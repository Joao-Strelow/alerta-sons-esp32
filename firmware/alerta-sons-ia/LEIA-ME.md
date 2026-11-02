# Firmware do ESP32 (IA própria)

Faz o WiFiManager, o heartbeat, o envio de eventos ao Supabase, os LEDs e os comandos
do Serial. O modelo de IA é o treinado no PC (pasta `ia/treino` do repositório) e roda no
ESP32 com **TensorFlow Lite Micro**.

## Arquivos

| Arquivo | Função |
|---|---|
| `alerta-sons-ia.ino` | Programa principal (rede, Supabase, tarefas, comandos) |
| `ia_sons.h` | Espectrograma mel + execução da rede neural (bem comentado) |
| `modelo_tflite.h` | **Gerado** pelo `ia/treino/exportar.py`: o modelo treinado |
| `config_features.h` | **Gerado** pelo `treino/exportar.py`: tabelas e parâmetros do espectrograma |

`modelo_tflite.h` e `config_features.h` não devem ser editados à mão. Para atualizar a IA:
retreinar (`treinar.py`), exportar (`exportar.py`) e copiar os dois arquivos para esta pasta.

## Bibliotecas (Arduino IDE → Gerenciar Bibliotecas)

- WiFiManager (tzapu)
- Adafruit NeoPixel
- **Chirale_TensorFLowLite** (TensorFlow Lite Micro para ESP32)

## Placa e partição

- Placa: ESP32 Dev Module.
- O programa ocupa ~99% da partição padrão. Para ter folga, em
  **Ferramentas → Partition Scheme** escolha **Huge APP (3MB No OTA/1MB SPIFFS)**.

## Coleta de novos sons (sem nuvem)

Os comandos `c N`, `p N` e `r N` do Serial gravam sons e enviam pelo cabo USB. Para salvar
no PC, feche o Serial Monitor e use `ia/treino/coletar_serial.py` (ver README da pasta `ia`).

## Ajuste da sensibilidade

`CONFIANCA_MIN` (0.95) define a confiança mínima para disparar. Use `l 1` no Serial para
ver as probabilidades em tempo real e ajustar: subir = menos falsos alertas, descer = menos
sons perdidos.
