// ============================================================================
// ALERTA DE SONS  -  firmware do ESP32 com IA PROPRIA
// ============================================================================
// O que o aparelho faz:
//   - escuta o microfone o tempo todo e reconhece "palma" e "campainha";
//   - pisca o LED e grava o evento na tabela "eventos" do Supabase;
//   - manda um heartbeat a cada 30 s para a tabela "nodes" (status online);
//   - WiFi configurado pelo portal "AlertaSons-Setup" (WiFiManager).
//
// O modelo de IA e o que treinamos no PC (pasta "ia/treino/" do repositorio),
// rodando com TensorFlow Lite Micro. O calculo do espectrograma e a inferencia
// estao em "ia_sons.h". Os arquivos "modelo_tflite.h" e "config_features.h"
// sao GERADOS pelo ia/treino/exportar.py (nao editar a mao).
//
// Bibliotecas necessarias (Arduino IDE -> Gerenciar Bibliotecas):
//   - WiFiManager (tzapu), Adafruit NeoPixel, Chirale_TensorFLowLite
// ============================================================================

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <HTTPClient.h>
#include <Adafruit_NeoPixel.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/i2s.h"
#include <strings.h>

#include "ia_sons.h"          // IA propria: espectrograma + rede neural

// ===================== CONFIGURAÇÕES =====================

// ----- Nó -----
#define NODE_ID "no1"                 // "no2" no segundo ESP32

// ----- WiFi -----
// Nao ha mais SSID/senha fixos no codigo: na primeira vez (ou apos "w" no
// serial), o ESP32 cria uma rede propria "AlertaSons-Setup" por alguns
// minutos. Conecte o celular nela, escolha a rede WiFi do lugar numa pagina
// que abre automaticamente, e pronto - fica salvo no proprio ESP32.
#define WIFI_AP_NOME "AlertaSons-Setup"
#define WIFI_PORTAL_TIMEOUT_S 180      // tempo que a rede de configuracao fica ligada

// ----- Supabase -----
const char* SUPABASE_URL = "https://talrflwkpjkzfanwnmwd.supabase.co";
const char* SUPABASE_KEY = "sb_publishable_ZQoMm-YfZSDjt02mHwFhpQ_QMIsXv87";
#define ENVIAR_SUPABASE_PADRAO false   // estado inicial (troque em tempo real com o comando "s 1" / "s 0")
#define HEARTBEAT_MS 30000             // intervalo do sinal de vida usado no status online/offline

// ----- Detecção -----
// Confianca minima para considerar uma deteccao. Nos testes em PC nao houve
// falso alerta de ruido a partir de 0.90. Ajuste olhando o log ("l 1").
#define CONFIANCA_MIN 0.95
#define COOLDOWN      5000            // ms mínimos entre dois eventos

// ----- Microfone INMP441 (I2S) -----
#define I2S_SCK 33
#define I2S_WS  25
#define I2S_SD  32
#define I2S_PORT ((i2s_port_t)0)
#define SAMPLE_RATE 16000
#define N_SAMPLES   16000             // 1 segundo (gravação de amostras para treino)
#define BLOCO       256
// Estes valores DEVEM ser iguais aos usados quando as amostras foram gravadas:
#define DESLOC 16
#define GANHO  1

// ----- Janela de análise da IA -----
// A IA analisa sempre o ULTIMO segundo de audio, e repete a analise a cada
// 250 ms (janela deslizante). O audio recente fica num "anel" (buffer
// circular) com 4000 amostras de folga, para a analise nunca ler um trecho
// que o microfone ja esta sobrescrevendo.
#define PASSO_JANELA 4000                    // 4000 amostras = 250 ms
#define TAM_ANEL     (IA_AMOSTRAS + 4000)    // 20000 amostras (40 KB)
#define JANELAS_POR_LOG 4                    // imprime as probabilidades a cada ~1 s

// ----- LED WS2812B -----
#define LED_PIN 4
#define LED_COUNT 2
Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

// ===================== ESTADO GLOBAL =====================

struct Evento {
  char tipo[16];
  float conf;
};

static bool record_status = true;

volatile bool capturaAtiva = true;
volatile bool capturaParada = false;
volatile bool acaoOcupada = false;

QueueHandle_t filaEventos;

bool enviarSupabase = ENVIAR_SUPABASE_PADRAO;
bool logPredicoes = true;
unsigned long ultimoEvento = 0;
int aquecimento = 1;            // 1 = ainda enchendo o primeiro segundo de audio
int contadorLog = 0;

// Buffer circular de audio (escrito pela tarefa do microfone, lido pelo loop).
// Reservado no heap, no inicio do setup (ver setup()), e nao como variavel
// global, porque 40 KB estouram a area de dados estaticos do ESP32.
static int16_t* anel = nullptr;
volatile uint32_t anelEscrita = 0;        // proxima posicao onde o microfone vai escrever
volatile uint32_t amostrasTotal = 0;      // amostras recebidas desde o ultimo reset
volatile uint32_t desdeUltimaJanela = 0;  // amostras desde a ultima janela pronta
volatile uint32_t janelaFim = 0;          // posicao do anel logo apos o fim da ultima janela pronta
volatile int janelasProntas = 0;          // janelas aguardando analise

// usados só durante a gravação de amostras
int ultimoPico = 0;
int ultimoRms = 0;

// ===================== LED =====================

void cor(uint8_t r, uint8_t g, uint8_t b) {
  for (int i = 0; i < LED_COUNT; i++) strip.setPixelColor(i, strip.Color(r, g, b));
  strip.show();
}

void piscar(uint8_t r, uint8_t g, uint8_t b, int vezes, int ms) {
  for (int i = 0; i < vezes; i++) {
    cor(r, g, b); delay(ms);
    cor(0, 0, 0); delay(ms);
  }
}

// ===================== WIFI =====================

// Abre o portal de configuracao (rede "AlertaSons-Setup") so quando preciso.
// Usado no boot e pelo comando serial "w" (trocar de rede manualmente).
bool abrirPortalWiFi() {
  WiFiManager wm;
  wm.setConfigPortalTimeout(WIFI_PORTAL_TIMEOUT_S);
  return wm.autoConnect(WIFI_AP_NOME);
}

// Reconexao rapida usando a ultima rede salva (sem abrir portal) - usada
// antes de cada chamada de rede, caso o WiFi tenha caido no meio do uso.
bool garantirWiFi(uint32_t timeoutMs) {
  if (WiFi.status() == WL_CONNECTED) return true;
  WiFi.mode(WIFI_STA);
  WiFi.begin();   // reusa a ultima rede/senha salva na memoria do ESP32
  uint32_t ini = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - ini < timeoutMs) {
    delay(300);
    Serial.print(".");
  }
  return WiFi.status() == WL_CONNECTED;
}

// ===================== SUPABASE =====================

SemaphoreHandle_t mutexRede;   // garante que so uma conexao HTTPS roda por vez (poupa RAM)

// Pausa so a captura de audio (sem reiniciar o classificador), liberando a CPU/RAM
// para a conexao HTTPS enquanto ela dura
void pausarCapturaLeve() {
  capturaAtiva = false;
  while (!capturaParada) delay(5);
}

void retomarCapturaLeve() {
  capturaAtiva = true;
}

int enviarEventoSupabase(const char* tipo, float conf) {
  if (!garantirWiFi(8000)) return -1;

  xSemaphoreTake(mutexRede, portMAX_DELAY);
  pausarCapturaLeve();

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  https.begin(client, String(SUPABASE_URL) + "/rest/v1/eventos");
  https.addHeader("apikey", SUPABASE_KEY);
  https.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);
  https.addHeader("Content-Type", "application/json");
  https.addHeader("Prefer", "return=minimal");

  long intensidade = (long)(conf * 100);
  String corpo = String("{\"node_id\":\"") + NODE_ID +
                 "\",\"tipo\":\"" + tipo +
                 "\",\"intensidade\":" + intensidade + "}";
  int code = https.POST(corpo);
  https.end();

  retomarCapturaLeve();
  xSemaphoreGive(mutexRede);
  return code;
}

// Grava/atualiza o "sinal de vida" deste no na tabela nodes (usado pelo app para status online/offline)
int enviarHeartbeatSupabase() {
  if (!garantirWiFi(8000)) return -1;

  xSemaphoreTake(mutexRede, portMAX_DELAY);
  pausarCapturaLeve();

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  https.begin(client, String(SUPABASE_URL) + "/rest/v1/nodes?on_conflict=node_id");
  https.addHeader("apikey", SUPABASE_KEY);
  https.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);
  https.addHeader("Content-Type", "application/json");
  https.addHeader("Prefer", "resolution=merge-duplicates,return=minimal");

  String corpo = String("{\"node_id\":\"") + NODE_ID + "\",\"last_seen\":\"now()\"}";
  int code = https.POST(corpo);
  https.end();

  retomarCapturaLeve();
  xSemaphoreGive(mutexRede);
  return code;
}

// Tarefa separada que manda o heartbeat periodicamente, sem travar a escuta do microfone
void tarefaHeartbeat(void* arg) {
  for (;;) {
    int code = enviarHeartbeatSupabase();
    Serial.printf("Heartbeat -> %d (heap livre %u, maior bloco %u)\n",
                  code, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_MS));
  }
}

// Tarefa que pisca o LED e envia ao Supabase sem travar a escuta do microfone
void tarefaAcoes(void* arg) {
  Evento ev;
  for (;;) {
    if (xQueueReceive(filaEventos, &ev, portMAX_DELAY) == pdTRUE) {
      acaoOcupada = true;
      bool ehPalma = (strcmp(ev.tipo, "palma") == 0);

      Serial.printf(">>> DETECTADO: %s (%.0f%%)\n", ev.tipo, ev.conf * 100);
      if (ehPalma) piscar(255, 0, 0, 3, 120);      // vermelho = palma
      else         piscar(255, 180, 0, 3, 120);    // amarelo = campainha

      if (enviarSupabase) {
        // o Supabase manda o alerta pro Telegram da pessoa sozinho
        // (webhook configurado no painel, ver README) - o firmware so grava o evento
        int code = enviarEventoSupabase(ev.tipo, ev.conf);
        Serial.printf("Supabase POST %s -> %d\n", ev.tipo, code);
        if (code >= 200 && code < 300) piscar(0, 200, 200, 1, 150);   // ciano = enviado
        else                           piscar(255, 0, 80, 2, 150);    // rosa = falhou
      }
      cor(0, 0, 0);
      acaoOcupada = false;
    }
  }
}

// ===================== I2S / MICROFONE =====================

void i2sInit() {
  i2s_config_t cfg = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 256,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0,
  };
  i2s_pin_config_t pins = {
    .bck_io_num = I2S_SCK,
    .ws_io_num = I2S_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = I2S_SD,
  };
  i2s_driver_install(I2S_PORT, &cfg, 0, NULL);
  i2s_set_pin(I2S_PORT, &pins);
  i2s_zero_dma_buffer(I2S_PORT);
}

// Tarefa que lê o microfone o tempo todo (pode ser pausada pelos comandos).
// Cada amostra vai para o "anel"; a cada 4000 amostras (250 ms) avisamos o
// loop() de que ha uma nova janela de 1 s pronta para a IA analisar.
static void capture_samples(void* arg) {
  static int32_t raw[BLOCO];
  size_t bytes_read = 0;

  while (record_status) {
    if (!capturaAtiva) {
      capturaParada = true;
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    capturaParada = false;

    bytes_read = 0;
    i2s_read(I2S_PORT, raw, sizeof(raw), &bytes_read, pdMS_TO_TICKS(100));
    if (bytes_read == 0) continue;

    int n = bytes_read / 4;
    for (int i = 0; i < n; i++) {
      int32_t v = (raw[i] >> DESLOC) * GANHO;
      if (v > 32767) v = 32767;
      if (v < -32768) v = -32768;

      uint32_t pos = anelEscrita;
      anel[pos] = (int16_t)v;
      pos++;
      if (pos >= TAM_ANEL) pos = 0;
      anelEscrita = pos;
      amostrasTotal = amostrasTotal + 1;

      desdeUltimaJanela = desdeUltimaJanela + 1;
      if (desdeUltimaJanela >= PASSO_JANELA) {
        desdeUltimaJanela = 0;
        janelaFim = pos;                     // a janela termina logo antes de "pos"
        janelasProntas = janelasProntas + 1;
      }
    }
  }
  vTaskDelete(NULL);
}

// ----- pausar / retomar a escuta para os comandos usarem o microfone -----

void aguardarAcoes() {
  while (acaoOcupada || uxQueueMessagesWaiting(filaEventos) > 0) delay(20);
  delay(50);
}

void pausarCaptura() {
  capturaAtiva = false;
  while (!capturaParada) delay(5);
}

void retomarCaptura() {
  i2s_zero_dma_buffer(I2S_PORT);
  anelEscrita = 0;
  amostrasTotal = 0;
  desdeUltimaJanela = 0;
  janelasProntas = 0;
  aquecimento = 1;            // precisa encher 1 segundo de audio novo antes de detectar
  contadorLog = 0;
  capturaAtiva = true;
}

// ===================== DETECÇÃO =====================

void tratarDeteccao(const char* label, float conf) {
  if (aquecimento > 0) return;
  if (conf < CONFIANCA_MIN) return;
  if (millis() - ultimoEvento < COOLDOWN) return;

  const char* tipo = NULL;
  if (strcasecmp(label, "palma") == 0) tipo = "palma";
  else if (strcasecmp(label, "campainha") == 0) tipo = "campainha";
  else return;   // "ruido" e outros: não faz nada

  ultimoEvento = millis();
  Evento ev;
  strncpy(ev.tipo, tipo, sizeof(ev.tipo) - 1);
  ev.tipo[sizeof(ev.tipo) - 1] = 0;
  ev.conf = conf;
  xQueueSend(filaEventos, &ev, 0);
}

// ===================== TESTE DO MIC (comando "m") =====================

void monitorarMic() {
  Serial.println("Monitorando o microfone. Faca barulho perto dele.");
  Serial.println("Envie qualquer tecla (ou Enter) para parar.");
  while (Serial.available()) Serial.read();

  int32_t raw[BLOCO];
  size_t br;
  while (!Serial.available()) {
    double soma = 0, somaQ = 0;
    long n = 0;
    int pico = 0;
    int32_t primeiro = 0;
    for (int b = 0; b < 8; b++) {            // 8 blocos de 256 amostras = ~128 ms
      i2s_read(I2S_PORT, raw, sizeof(raw), &br, portMAX_DELAY);
      size_t k = br / 4;
      if (b == 0 && k > 0) primeiro = raw[0];
      for (size_t i = 0; i < k; i++) {
        int32_t v = (raw[i] >> DESLOC) * GANHO;
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        int a = v < 0 ? -v : v;
        if (a > pico) pico = a;
        soma += v;
        somaQ += (double)v * v;
        n++;
      }
    }
    double media = soma / n;
    double var = somaQ / n - media * media;
    int rms = (int)sqrt(var > 0 ? var : 0);
    int barras = (int)(log10((double)rms + 1) * 10);   // escala logarítmica
    if (barras > 50) barras = 50;
    char barra[52];
    for (int i = 0; i < barras; i++) barra[i] = '#';
    barra[barras] = 0;
    Serial.printf("rms=%5d pico=%5d raw0=0x%08X %s%s\n", rms, pico, (unsigned int)primeiro,
                  barra, pico >= 32767 ? " <- ESTOURADO" : "");
  }
  while (Serial.available()) Serial.read();
  Serial.println("Monitor parado.");
}

// ===================== GRAVAR AMOSTRAS PARA TREINO (via cabo USB) =====================
// Sem nuvem: o ESP32 grava 1 segundo de audio e envia pelo cabo USB para o PC,
// onde o script "treino/coletar_serial.py" salva o arquivo .wav na pasta dados/.
// Protocolo (o script do PC procura estas marcas):
//     <<WAV classe tamanho_em_bytes>>\n  +  bytes do audio (int16, little-endian)  +  \n<<FIM>>\n

// Descarta ~0,5s de áudio: limpa o buffer antigo e deixa o mic estabilizar
void descartar() {
  size_t br;
  int32_t raw[BLOCO];
  for (int i = 0; i < 32; i++) i2s_read(I2S_PORT, raw, sizeof(raw), &br, portMAX_DELAY);
}

// Grava 1 segundo e envia pela serial. Usa o "anel" como area de gravacao
// (a escuta esta pausada durante os comandos, entao ele esta livre).
void gravarAmostraSerial(const char* label, bool pausaExtra) {
  cor(140, 0, 255);                   // roxo = prepare-se (ainda NAO grava)
  descartar();
  if (pausaExtra) delay(700);

  cor(255, 255, 255);                 // branco = GRAVANDO 1s, faca o som agora
  int32_t raw[BLOCO];
  size_t br, total = 0;
  int pico = 0;
  double soma = 0;
  while (total < N_SAMPLES) {
    i2s_read(I2S_PORT, raw, sizeof(raw), &br, portMAX_DELAY);
    size_t n = br / 4;
    for (size_t i = 0; i < n && total < N_SAMPLES; i++) {
      int32_t v = (raw[i] >> DESLOC) * GANHO;
      if (v > 32767) v = 32767;
      if (v < -32768) v = -32768;
      int a = v < 0 ? -v : v;
      if (a > pico) pico = a;
      soma += (double)v * v;
      anel[total++] = (int16_t)v;
    }
  }
  ultimoPico = pico;
  ultimoRms = (int)sqrt(soma / N_SAMPLES);

  cor(0, 0, 255);                     // azul = enviando ao PC
  Serial.printf("<<WAV %s %u>>\n", label, (unsigned)(N_SAMPLES * 2));
  Serial.write((const uint8_t*)anel, N_SAMPLES * 2);
  Serial.print("\n<<FIM>>\n");
  Serial.flush();
}

void gravarParaTreino(const char* nome, int qtd) {
  for (int i = 0; i < qtd; i++) {
    gravarAmostraSerial(nome, qtd == 1);
    Serial.printf("[%d/%d] %s | pico=%d rms=%d%s\n", i + 1, qtd, nome, ultimoPico, ultimoRms,
                  ultimoPico >= 32767 ? " <- ESTOURADO" : "");
    cor(0, 255, 0); delay(150);       // verde = ok
    cor(0, 0, 0);
  }
  Serial.println("Pronto.");
}

// ===================== COMANDOS =====================

void mostrarAjuda() {
  Serial.println("-------- COMANDOS --------");
  Serial.println("s 1 / s 0  = liga / desliga envio ao Supabase (s = mostra estado)");
  Serial.println("w          = esquece a rede WiFi atual e abre o portal de configuracao de novo");
  Serial.println("m          = monitorar microfone (qualquer tecla para parar)");
  Serial.println("c N        = gravar N amostras de campainha (enviadas ao PC pelo USB)");
  Serial.println("p N        = gravar N amostras de palma (enviadas ao PC pelo USB)");
  Serial.println("r N        = gravar N amostras de ruido (enviadas ao PC pelo USB)");
  Serial.println("            (use o script treino/coletar_serial.py para salvar no PC)");
  Serial.println("l 1 / l 0  = liga / desliga log das probabilidades");
  Serial.println("?          = mostra esta ajuda");
  Serial.println("--------------------------");
  Serial.printf("Envio ao Supabase: %s | Log: %s\n",
                enviarSupabase ? "LIGADO" : "DESLIGADO", logPredicoes ? "LIGADO" : "DESLIGADO");
}

void processarComando(String linha) {
  linha.trim();
  linha.toLowerCase();
  if (linha.length() == 0) return;

  String nome = linha;
  String arg = "";
  int esp = linha.indexOf(' ');
  if (esp > 0) {
    nome = linha.substring(0, esp);
    arg = linha.substring(esp + 1);
    arg.trim();
  }

  if (nome == "?" || nome == "h") { mostrarAjuda(); return; }

  if (nome == "w") {
    Serial.println("Esquecendo a rede WiFi atual. Reiniciando para abrir o portal...");
    WiFiManager wm;
    wm.resetSettings();
    delay(500);
    ESP.restart();
    return;
  }

  if (nome == "s") {
    if (arg == "1" || arg == "on") enviarSupabase = true;
    else if (arg == "0" || arg == "off") enviarSupabase = false;
    Serial.printf("Envio ao Supabase: %s\n", enviarSupabase ? "LIGADO" : "DESLIGADO");
    return;
  }

  if (nome == "l") {
    if (arg == "1" || arg == "on") logPredicoes = true;
    else if (arg == "0" || arg == "off") logPredicoes = false;
    else logPredicoes = !logPredicoes;
    Serial.printf("Log das probabilidades: %s\n", logPredicoes ? "LIGADO" : "DESLIGADO");
    return;
  }

  if (nome == "m") {
    aguardarAcoes();
    pausarCaptura();
    monitorarMic();
    retomarCaptura();
    return;
  }

  if (nome == "c" || nome == "p" || nome == "r") {
    int qtd = arg.toInt();
    if (qtd < 1) qtd = 1;
    const char* label = (nome == "c") ? "campainha" : (nome == "p") ? "palma" : "ruido";
    aguardarAcoes();
    xSemaphoreTake(mutexRede, portMAX_DELAY);   // pausa heartbeat/Supabase (evita mensagens no meio do envio)
    pausarCaptura();
    gravarParaTreino(label, qtd);
    retomarCaptura();
    xSemaphoreGive(mutexRede);                  // heartbeat/Supabase voltam ao normal
    return;
  }

  Serial.println("Comando desconhecido. Digite ? para ver a ajuda.");
}

// ===================== SETUP / LOOP =====================

void setup() {
  Serial.begin(115200);
  delay(500);

  strip.begin();
  strip.setBrightness(80);
  cor(0, 0, 0);

  // Reserva a memoria grande ANTES de ligar o WiFi (evita fragmentacao do heap,
  // que prejudicaria as conexoes HTTPS depois).
  anel = (int16_t*)heap_caps_malloc(TAM_ANEL * sizeof(int16_t), MALLOC_CAP_8BIT);
  if (anel == nullptr) {
    Serial.println("ERRO: sem memoria para o buffer de audio.");
    for (;;) { piscar(255, 0, 80, 1, 300); }
  }
  memset(anel, 0, TAM_ANEL * sizeof(int16_t));

  if (!ia_iniciar()) {
    Serial.println("ERRO: nao foi possivel iniciar a IA.");
    for (;;) { piscar(255, 0, 80, 1, 300); }
  }
  Serial.printf("IA propria: %d classes | modelo %u bytes | arena usada %u de %u bytes\n",
                IA_N_CLASSES, modelo_tflite_len, (unsigned)ia_arena_usada(), (unsigned)IA_TAMANHO_ARENA);

  cor(0, 0, 255);   // azul = conectando/configurando WiFi
  Serial.println("Conectando WiFi (ou abrindo portal de configuracao)...");
  if (abrirPortalWiFi()) {
    Serial.println("WiFi conectado.");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("Sem WiFi por agora. Vou tentar de novo quando precisar enviar algo.");
    Serial.println("Envie 'w' no serial para abrir o portal de configuracao manualmente.");
  }
  cor(0, 0, 0);

  i2sInit();
  mutexRede = xSemaphoreCreateMutex();
  filaEventos = xQueueCreate(4, sizeof(Evento));
  xTaskCreate(capture_samples, "Captura", 8192, NULL, 10, NULL);
  xTaskCreate(tarefaAcoes, "Acoes", 8192, NULL, 1, NULL);
  xTaskCreate(tarefaHeartbeat, "Heartbeat", 6144, NULL, 1, NULL);

  piscar(0, 255, 0, 2, 150);   // verde 2x = pronto
  mostrarAjuda();
  Serial.println("Escutando...");
}

void loop() {
  if (Serial.available()) processarComando(Serial.readStringUntil('\n'));

  // espera o microfone completar mais 250 ms de audio
  while (janelasProntas == 0) delay(1);
  if (janelasProntas > 1) {
    Serial.println("Aviso: analise atrasada (overrun).");
  }
  uint32_t fim = janelaFim;
  janelasProntas = 0;

  // so detecta depois de ter 1 segundo inteiro de audio novo
  aquecimento = (amostrasTotal < (uint32_t)IA_AMOSTRAS) ? 1 : 0;

  // 1) espectrograma do ultimo segundo  2) rede neural  -> probabilidades
  int inicio = (int)((fim + TAM_ANEL - IA_AMOSTRAS) % TAM_ANEL);
  ia_preencher_entrada(anel, TAM_ANEL, inicio);

  float probs[IA_N_CLASSES];
  if (!ia_classificar(probs)) {
    Serial.println("ERRO ao classificar.");
    return;
  }

  // classe com maior probabilidade nesta janela
  float melhorValor = 0;
  const char* melhorLabel = "";
  for (int ix = 0; ix < IA_N_CLASSES; ix++) {
    if (probs[ix] > melhorValor) {
      melhorValor = probs[ix];
      melhorLabel = IA_CLASSES[ix];
    }
  }
  tratarDeteccao(melhorLabel, melhorValor);

  if (aquecimento == 0 && ++contadorLog >= JANELAS_POR_LOG) {
    if (logPredicoes) {
      for (int ix = 0; ix < IA_N_CLASSES; ix++) {
        Serial.printf("%s=%.2f  ", IA_CLASSES[ix], probs[ix]);
      }
      Serial.println();
    }
    contadorLog = 0;
  }
}
