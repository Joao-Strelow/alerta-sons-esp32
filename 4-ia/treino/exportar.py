"""
exportar.py  -  PREPARA O MODELO PARA O ESP32
==============================================

Como executar (depois de treinar):

    .venv\\Scripts\\python.exe treino\\exportar.py

Um modelo treinado no computador usa números decimais de 32 bits ("float32") e
é grande demais/lento para um microcontrolador. Este script faz três coisas:

  1. QUANTIZAÇÃO: converte o modelo para números inteiros de 8 bits (int8).
     O arquivo fica ~4x menor e a conta fica mais rápida, com pouca perda de
     precisão. Para isso o conversor precisa de alguns exemplos reais
     ("dados representativos") para descobrir a faixa de valores de cada camada.

  2. TESTE: mede a acurácia do modelo reduzido nos áudios de teste e compara
     com o modelo original. Se cair muito, algo está errado.

  3. GERA ARQUIVOS .h (código C++) que são copiados para o projeto do ESP32:
       - modelo_tflite.h   : o modelo em si, escrito como uma lista de bytes
       - config_features.h : parâmetros do espectrograma + tabelas (janela de
                             Hann e filtros mel) + média/desvio da normalização.
                             Com isso o ESP32 calcula as features IGUAL ao Python.
"""

import json
import os
import shutil

os.environ["TF_CPP_MIN_LOG_LEVEL"] = "2"

import numpy as np
import tensorflow as tf

import config
import features


# ---------------------------------------------------------------------------
# 1) QUANTIZAÇÃO (float32 -> int8)
# ---------------------------------------------------------------------------
def converter_para_tflite_int8(modelo, exemplos):
    """Converte o modelo Keras num modelo TensorFlow Lite totalmente int8."""

    def gerador_representativo():
        # O conversor "assiste" a estes exemplos passando pela rede para medir
        # os valores mínimo/máximo de cada camada e escolher a escala do int8.
        for x in exemplos:
            yield [x[np.newaxis, ...].astype(np.float32)]

    # No ESP32 o modelo sempre processa UM áudio por vez. Fixar o tamanho do lote
    # em 1 faz o conversor gerar um modelo mais simples (sem operações extras
    # que calculariam o tamanho do lote durante a execução).
    entrada = tf.keras.Input(shape=config.FORMATO_ENTRADA, batch_size=1)
    modelo_lote_1 = tf.keras.Model(entrada, modelo(entrada))

    conversor = tf.lite.TFLiteConverter.from_keras_model(modelo_lote_1)
    conversor.optimizations = [tf.lite.Optimize.DEFAULT]
    conversor.representative_dataset = gerador_representativo
    conversor.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    conversor.inference_input_type = tf.int8      # entrada do modelo já em int8
    conversor.inference_output_type = tf.int8     # saída do modelo em int8
    return conversor.convert()


# ---------------------------------------------------------------------------
# 2) TESTE DO MODELO REDUZIDO
# ---------------------------------------------------------------------------
def avaliar_tflite(modelo_tflite, x_teste, y_teste):
    """Roda o modelo int8 em todos os exemplos de teste e devolve a acurácia."""
    interpretador = tf.lite.Interpreter(model_content=modelo_tflite)
    interpretador.allocate_tensors()
    entrada = interpretador.get_input_details()[0]
    saida = interpretador.get_output_details()[0]
    escala, zero = entrada["quantization"]          # como converter float <-> int8

    acertos = 0
    for x, y in zip(x_teste, y_teste):
        # float -> int8:  q = x / escala + zero   (limitado a -128..127)
        q = np.clip(np.round(x / escala + zero), -128, 127).astype(np.int8)
        interpretador.set_tensor(entrada["index"], q[np.newaxis, ...])
        interpretador.invoke()
        previsto = int(interpretador.get_tensor(saida["index"]).argmax())
        acertos += int(previsto == y)
    return acertos / len(y_teste), entrada, saida


# ---------------------------------------------------------------------------
# 3) GERAÇÃO DOS ARQUIVOS .h
# ---------------------------------------------------------------------------
def literal_float(valor):
    """
    Escreve um número como literal float do C++ (9 dígitos = precisão exata do float32).
    O C++ exige um ponto decimal antes do "f": "0f" é inválido, "0.0f" é válido.
    """
    texto = f"{float(valor):.9g}"
    if "." not in texto and "e" not in texto:
        texto += ".0"
    return texto + "f"


def formatar_floats(valores, por_linha=8):
    """Texto C++ com uma lista de floats."""
    itens = [literal_float(v) for v in np.asarray(valores).ravel()]
    linhas = [", ".join(itens[i:i + por_linha]) for i in range(0, len(itens), por_linha)]
    return ",\n  ".join(linhas)


def gerar_modelo_h(modelo_tflite, caminho):
    """Escreve o arquivo .tflite como um vetor de bytes em C++."""
    bytes_ = list(modelo_tflite)
    linhas = []
    for i in range(0, len(bytes_), 16):
        linhas.append(", ".join(f"0x{b:02x}" for b in bytes_[i:i + 16]))
    corpo = ",\n  ".join(linhas)
    with open(caminho, "w") as f:
        f.write("// GERADO AUTOMATICAMENTE por treino/exportar.py - não editar à mão.\n")
        f.write("// Modelo TensorFlow Lite (int8) da IA de reconhecimento de sons.\n")
        f.write("#pragma once\n\n")
        # alignas(16): o TensorFlow Lite Micro exige o modelo alinhado na memória.
        f.write(f"alignas(16) const unsigned char modelo_tflite[] = {{\n  {corpo}\n}};\n")
        f.write(f"const unsigned int modelo_tflite_len = {len(bytes_)};\n")


def gerar_config_features_h(caminho, media, desvio, entrada, saida):
    """Escreve os parâmetros e tabelas que o ESP32 precisa para calcular as features."""
    escala_in, zero_in = entrada["quantization"]
    escala_out, zero_out = saida["quantization"]
    classes = ", ".join(f'"{c}"' for c in config.CLASSES)
    n_bins = config.JANELA_FFT // 2 + 1

    with open(caminho, "w") as f:
        f.write("// GERADO AUTOMATICAMENTE por treino/exportar.py - não editar à mão.\n")
        f.write("// Parâmetros para o ESP32 calcular o espectrograma IGUAL ao do treino em Python.\n")
        f.write("#pragma once\n\n")
        f.write(f"#define IA_TAXA          {config.TAXA_AMOSTRAGEM}\n")
        f.write(f"#define IA_AMOSTRAS      {config.AMOSTRAS_POR_CLIPE}   // 1 segundo de áudio\n")
        f.write(f"#define IA_JANELA_FFT    {config.JANELA_FFT}\n")
        f.write(f"#define IA_SALTO         {config.SALTO}\n")
        f.write(f"#define IA_BANDAS_MEL    {config.BANDAS_MEL}\n")
        f.write(f"#define IA_N_BINS        {n_bins}\n")
        f.write(f"#define IA_N_QUADROS     {config.N_QUADROS}\n")
        f.write(f"#define IA_N_CLASSES     {len(config.CLASSES)}\n")
        f.write(f"#define IA_EPSILON       {literal_float(config.EPSILON)}\n\n")
        f.write("// Normalização aplicada às features: (x - media) / desvio\n")
        f.write(f"static const float IA_MEDIA  = {literal_float(media)};\n")
        f.write(f"static const float IA_DESVIO = {literal_float(desvio)};\n\n")
        f.write("// Conversão float <-> int8 da entrada e da saída do modelo quantizado\n")
        f.write(f"static const float IA_ENTRADA_ESCALA = {literal_float(escala_in)};\n")
        f.write(f"static const int   IA_ENTRADA_ZERO   = {int(zero_in)};\n")
        f.write(f"static const float IA_SAIDA_ESCALA   = {literal_float(escala_out)};\n")
        f.write(f"static const int   IA_SAIDA_ZERO     = {int(zero_out)};\n\n")
        f.write("// Ordem das classes na saída do modelo\n")
        f.write(f"static const char* const IA_CLASSES[IA_N_CLASSES] = {{ {classes} }};\n\n")
        f.write(f"// Janela de Hann ({config.JANELA_FFT} valores)\n")
        f.write(f"static const float IA_JANELA_HANN[IA_JANELA_FFT] = {{\n  {formatar_floats(features.JANELA_HANN)}\n}};\n\n")
        f.write(f"// Filtros mel: {config.BANDAS_MEL} linhas x {n_bins} colunas, linha a linha\n")
        f.write(f"static const float IA_FILTROS_MEL[IA_BANDAS_MEL * IA_N_BINS] = {{\n  {formatar_floats(features.FILTROS_MEL)}\n}};\n")


# ---------------------------------------------------------------------------
# PROGRAMA PRINCIPAL
# ---------------------------------------------------------------------------
def main():
    pasta = config.PASTA_MODELO
    modelo = tf.keras.models.load_model(pasta / "modelo_keras.keras")
    norm = json.load(open(pasta / "normalizacao.json"))
    dados = np.load(pasta / "dados_exportacao.npz")
    x_teste, y_teste = dados["x_teste"], dados["y_teste"]

    # Acurácia do modelo original (float32), para comparar.
    prev = modelo.predict(x_teste, verbose=0).argmax(axis=1)
    acc_float = float((prev == y_teste).mean())

    print("1) Quantizando para int8...")
    tflite = converter_para_tflite_int8(modelo, dados["representativos"])
    (pasta / "modelo_alerta.tflite").write_bytes(tflite)

    print("2) Testando o modelo reduzido...")
    acc_int8, entrada, saida = avaliar_tflite(tflite, x_teste, y_teste)

    print("3) Gerando arquivos .h para o ESP32...")
    gerar_modelo_h(tflite, pasta / "modelo_tflite.h")
    gerar_config_features_h(pasta / "config_features.h", norm["media"], norm["desvio"], entrada, saida)

    resumo = (f"Acurácia no teste - modelo original (float32): {acc_float * 100:.1f}%\n"
              f"Acurácia no teste - modelo reduzido (int8)   : {acc_int8 * 100:.1f}%\n"
              f"Tamanho do modelo int8: {len(tflite) / 1024:.1f} KB\n")
    (pasta / "relatorio_exportacao.txt").write_text(resumo, encoding="utf-8")
    print("\n" + resumo)
    print(f"Arquivos gerados em: {pasta}")

    # 4) Copia os dois .h para o firmware (antes era um passo manual)
    if config.PASTA_FIRMWARE.is_dir():
        for nome in ("modelo_tflite.h", "config_features.h"):
            shutil.copy2(pasta / nome, config.PASTA_FIRMWARE / nome)
        print(f"Copiados para o firmware: {config.PASTA_FIRMWARE}")
        print("Agora abra o .ino na Arduino IDE e grave de novo no ESP32.")
    else:
        print("Pasta do firmware não encontrada: copie os .h manualmente.")


if __name__ == "__main__":
    main()
