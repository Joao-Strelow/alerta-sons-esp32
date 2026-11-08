// ============================================================================
// ia_sons.h  -  IA PRÓPRIA DO PROJETO, RODANDO NO ESP32
// ============================================================================
//
// Este arquivo faz DUAS coisas, exatamente como foi feito no treino em Python
// (pasta treino/ do projeto):
//
//   1) Transforma 1 segundo de áudio em um espectrograma mel logarítmico
//      (uma "imagem" 49 x 32 do som)             -> ia_preencher_entrada()
//   2) Roda a rede neural (TensorFlow Lite Micro) sobre essa imagem e devolve
//      a probabilidade de cada classe              -> ia_classificar()
//
// IMPORTANTE: os passos da etapa 1 precisam ser IDÊNTICOS aos de
// treino/features.py. Se algo for diferente, o modelo "enxerga" um espectrograma
// diferente do que aprendeu e passa a errar. Os números (janela de Hann,
// filtros mel, média e desvio) vêm do arquivo config_features.h, gerado
// automaticamente pelo treino/exportar.py.
// ============================================================================

#pragma once

#include <Arduino.h>
#include <math.h>
#include "esp_heap_caps.h"     // reserva de memoria (heap_caps_malloc)

#include <Chirale_TensorFlowLite.h>                      // biblioteca TensorFlow Lite Micro
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/schema/schema_generated.h"

#include "config_features.h"   // parâmetros + tabelas do espectrograma (gerado pelo Python)
#include "modelo_tflite.h"     // o modelo treinado, como lista de bytes (gerado pelo Python)

// ----------------------------------------------------------------------------
// MEMÓRIA DA REDE NEURAL
// ----------------------------------------------------------------------------
// A "arena" é a área de RAM onde o TensorFlow Lite guarda os resultados
// intermediários de cada camada. O tamanho ideal é descoberto por tentativa:
// o firmware mostra no Serial quanto foi realmente usado (ia_arena_usada()).
// Ela e reservada no heap (e nao como variavel global) porque as variaveis
// globais do ESP32 tem um limite pequeno de memoria; o heap tem bem mais espaco.
#define IA_TAMANHO_ARENA (24 * 1024)
static uint8_t* ia_arena = nullptr;

static tflite::MicroInterpreter* ia_interp = nullptr;   // executa o modelo
static TfLiteTensor* ia_entrada = nullptr;               // onde colocamos o espectrograma
static TfLiteTensor* ia_saida = nullptr;                 // de onde lemos as probabilidades

// ----------------------------------------------------------------------------
// TABELAS PARA A FFT (calculadas uma vez, no início)
// ----------------------------------------------------------------------------
static float ia_cos[IA_JANELA_FFT / 2];        // cos(2*pi*k/N)
static float ia_sin[IA_JANELA_FFT / 2];        // sin(2*pi*k/N)
static uint16_t ia_bitrev[IA_JANELA_FFT];      // ordem "bit-reversa" que a FFT exige

// Áreas de trabalho da FFT (estáticas para não gastar a pilha da tarefa)
static float ia_re[IA_JANELA_FFT];             // parte real
static float ia_im[IA_JANELA_FFT];             // parte imaginária
static float ia_pot[IA_N_BINS];                // potência de cada frequência

// ----------------------------------------------------------------------------
// FFT (Transformada Rápida de Fourier), algoritmo radix-2
// ----------------------------------------------------------------------------
// A FFT descobre quanta energia existe em cada frequência dentro de um pedaço
// de 512 amostras. Trabalha "no lugar": o resultado substitui ia_re / ia_im.
static void ia_fft() {
  const int N = IA_JANELA_FFT;

  // 1) Reordena as amostras (permutação bit-reversa), preparação do algoritmo.
  for (int i = 0; i < N; i++) {
    int j = ia_bitrev[i];
    if (j > i) {
      float t = ia_re[i]; ia_re[i] = ia_re[j]; ia_re[j] = t;
      t = ia_im[i]; ia_im[i] = ia_im[j]; ia_im[j] = t;
    }
  }

  // 2) Combina blocos pequenos em blocos maiores (2, 4, 8 ... 512 pontos).
  //    Cada combinação é uma "borboleta" que mistura dois valores.
  for (int tam = 2; tam <= N; tam <<= 1) {
    int meio = tam >> 1;
    int passo = N / tam;
    for (int i = 0; i < N; i += tam) {
      for (int k = 0; k < meio; k++) {
        float wr = ia_cos[k * passo];       // fator de rotação: e^(-i*2*pi*k/tam)
        float wi = -ia_sin[k * passo];
        int a = i + k;
        int b = a + meio;
        float tr = ia_re[b] * wr - ia_im[b] * wi;
        float ti = ia_re[b] * wi + ia_im[b] * wr;
        ia_re[b] = ia_re[a] - tr;
        ia_im[b] = ia_im[a] - ti;
        ia_re[a] += tr;
        ia_im[a] += ti;
      }
    }
  }
}

// ----------------------------------------------------------------------------
// ESPECTROGRAMA: áudio -> entrada da rede neural
// ----------------------------------------------------------------------------
// Lê 1 segundo de áudio de um "anel" (buffer circular de int16) e escreve o
// espectrograma, já normalizado e convertido para int8, direto no tensor de
// entrada do modelo.
//
//   anel   : buffer circular com o áudio recente
//   tam    : tamanho do buffer circular
//   inicio : posição do anel onde começa o segundo de áudio a analisar
//
// Passos (os mesmos de treino/features.py), para cada um dos 49 "quadros":
//   a) pega 512 amostras e converte de inteiro para decimal (-1.0 a +1.0)
//   b) multiplica pela janela de Hann (suaviza as bordas)
//   c) aplica a FFT e calcula a potência de cada frequência (real² + imag²)
//   d) soma as frequências em 32 faixas mel (usando a tabela de filtros)
//   e) aplica o logaritmo natural
//   f) normaliza: (valor - média) / desvio
//   g) converte para int8 (a rede quantizada trabalha com inteiros de 8 bits)
static void ia_preencher_entrada(const int16_t* anel, int tam, int inicio) {
  int8_t* destino = ia_entrada->data.int8;

  for (int q = 0; q < IA_N_QUADROS; q++) {
    // posição do primeiro sample deste quadro (com volta ao início do anel)
    int pos = (inicio + q * IA_SALTO) % tam;

    // a) e b) copia 512 amostras já multiplicadas pela janela de Hann
    for (int j = 0; j < IA_JANELA_FFT; j++) {
      int idx = pos + j;
      if (idx >= tam) idx -= tam;
      ia_re[j] = (anel[idx] / 32768.0f) * IA_JANELA_HANN[j];
      ia_im[j] = 0.0f;
    }

    // c) FFT e potência (só as 257 primeiras frequências importam)
    ia_fft();
    for (int k = 0; k < IA_N_BINS; k++) {
      ia_pot[k] = ia_re[k] * ia_re[k] + ia_im[k] * ia_im[k];
    }

    // d), e), f), g) para cada faixa mel
    for (int m = 0; m < IA_BANDAS_MEL; m++) {
      const float* filtro = &IA_FILTROS_MEL[m * IA_N_BINS];
      float soma = 0.0f;
      for (int k = 0; k < IA_N_BINS; k++) soma += filtro[k] * ia_pot[k];

      float x = logf(soma + IA_EPSILON);          // e) logaritmo
      x = (x - IA_MEDIA) / IA_DESVIO;             // f) normalização

      // g) float -> int8:  q = x / escala + zero   (limitado a -128..127)
      int v = (int)lroundf(x / IA_ENTRADA_ESCALA) + IA_ENTRADA_ZERO;
      if (v > 127) v = 127;
      if (v < -128) v = -128;
      destino[q * IA_BANDAS_MEL + m] = (int8_t)v;
    }
  }
}

// ----------------------------------------------------------------------------
// INFERÊNCIA: roda a rede neural
// ----------------------------------------------------------------------------
// Devolve em probs[] a probabilidade (0.0 a 1.0) de cada classe, na mesma
// ordem de IA_CLASSES: campainha, palma, ruido. Retorna false se der erro.
static bool ia_classificar(float* probs) {
  if (ia_interp->Invoke() != kTfLiteOk) return false;

  for (int c = 0; c < IA_N_CLASSES; c++) {
    // A saída vem em int8; converte de volta para probabilidade decimal:
    //   p = (valor - zero) * escala
    probs[c] = (ia_saida->data.int8[c] - IA_SAIDA_ZERO) * IA_SAIDA_ESCALA;
  }
  return true;
}

static size_t ia_arena_usada() {
  return ia_interp ? ia_interp->arena_used_bytes() : 0;
}

// ----------------------------------------------------------------------------
// INICIALIZAÇÃO (chamar uma vez no setup)
// ----------------------------------------------------------------------------
static bool ia_iniciar() {
  // --- tabelas da FFT ---
  const int N = IA_JANELA_FFT;
  for (int k = 0; k < N / 2; k++) {
    float ang = 2.0f * (float)M_PI * k / N;
    ia_cos[k] = cosf(ang);
    ia_sin[k] = sinf(ang);
  }
  int bits = 0;
  while ((1 << bits) < N) bits++;
  for (int i = 0; i < N; i++) {
    int r = 0;
    for (int b = 0; b < bits; b++) if (i & (1 << b)) r |= 1 << (bits - 1 - b);
    ia_bitrev[i] = r;
  }

  // --- reserva a memória da rede neural (arena) ---
  ia_arena = (uint8_t*)heap_caps_malloc(IA_TAMANHO_ARENA, MALLOC_CAP_8BIT);
  if (ia_arena == nullptr) {
    Serial.println("IA: sem memoria para a arena.");
    return false;
  }

  // --- carrega o modelo (não copia: apenas aponta para os bytes na memória flash) ---
  const tflite::Model* modelo = tflite::GetModel(modelo_tflite);
  if (modelo->version() != TFLITE_SCHEMA_VERSION) {
    Serial.println("IA: versão do modelo incompatível com a biblioteca.");
    return false;
  }

  // --- registra só as operações que o nosso modelo usa (economiza memória) ---
  static tflite::MicroMutableOpResolver<5> resolver;
  resolver.AddConv2D();           // camadas convolucionais
  resolver.AddMaxPool2D();        // redução do tamanho da imagem
  resolver.AddReshape();          // "estica" a imagem num vetor
  resolver.AddFullyConnected();   // camada final (densa)
  resolver.AddSoftmax();          // transforma em probabilidades

  // --- cria o interpretador e reserva a memória das camadas ---
  static tflite::MicroInterpreter interpretador(modelo, resolver, ia_arena, IA_TAMANHO_ARENA);
  ia_interp = &interpretador;
  if (ia_interp->AllocateTensors() != kTfLiteOk) {
    Serial.println("IA: arena pequena demais (aumente IA_TAMANHO_ARENA).");
    return false;
  }

  ia_entrada = ia_interp->input(0);
  ia_saida = ia_interp->output(0);

  // confere se o modelo é do tipo esperado (int8, 49 x 32)
  if (ia_entrada->type != kTfLiteInt8 || ia_entrada->bytes != IA_N_QUADROS * IA_BANDAS_MEL) {
    Serial.println("IA: formato de entrada do modelo inesperado.");
    return false;
  }
  return true;
}
