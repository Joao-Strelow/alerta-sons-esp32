# 3 · Aparelho (ESP32)

**Para que serve:** é o aparelho em si. Escuta o microfone, roda a IA, acende o LED e envia o evento ao
Supabase. Tudo está na pasta [`alerta-sons-ia/`](alerta-sons-ia/), que é um *sketch* do Arduino.

## O que tem aqui

| Arquivo | Função | Posso editar? |
|---|---|---|
| `alerta-sons-ia.ino` | Programa principal: Wi-Fi, Supabase, tarefas, LED, comandos do Serial | Sim |
| `ia_sons.h` | Espectrograma mel + execução da rede neural (bem comentado) | Raramente |
| `secrets.h` | **Sua configuração:** nome do aparelho, URL e chave do Supabase | Sim (criado pelo setup) |
| `secrets.h.example` | Modelo do `secrets.h` | — |
| `modelo_tflite.h` | O modelo treinado como lista de bytes | **Não:** gerado pelo `4-ia/treino/exportar.py` |
| `config_features.h` | Tabelas e parâmetros do espectrograma | **Não:** gerado pelo `exportar.py` |

## O que configurar

Só o `secrets.h` (o `setup.bat` cria para você, ou copie o `secrets.h.example`):

| Variável | Significado |
|---|---|
| `NODE_ID` | Nome do aparelho (`no1`; use outro em cada ESP32) |
| `SUPABASE_URL` / `SUPABASE_KEY` | Os mesmos valores do site |
| `ENVIAR_SUPABASE_PADRAO` | `true` = já começa enviando; `false` = começa desligado (liga com `s 1`) |

Ajustes de detecção no topo do `.ino`: `CONFIANCA_MIN` (0.95) e `COOLDOWN` (5000 ms). Veja a
seção "Ajustar a sensibilidade" abaixo.

## Montagem

| INMP441 (microfone) | ESP32 |
|---|---|
| VDD | 3V3 |
| GND | GND |
| SCK | GPIO 33 |
| WS | GPIO 25 |
| SD | GPIO 32 |
| L/R | GND |

| LED WS2812B (2 LEDs) | ESP32 |
|---|---|
| DIN | GPIO 4 |
| 5V | 5V / VIN |
| GND | GND |

## Gravar no ESP32 (Arduino IDE 2.x)

1. **Placa:** em *Gerenciador de Placas* instale `esp32` (Espressif), versão 3.x.
2. **Bibliotecas** (*Gerenciar Bibliotecas*): `WiFiManager` (tzapu), `Adafruit NeoPixel` e
   `Chirale_TensorFLowLite`.
3. Abra `alerta-sons-ia/alerta-sons-ia.ino`.
4. *Ferramentas*: Placa **ESP32 Dev Module**; **Partition Scheme: Huge APP (3MB No OTA/1MB SPIFFS)**
   (o programa ocupa 99% da partição padrão); escolha a **Porta** COM.
5. Clique em **Carregar**. Se travar em "Connecting...", segure o botão **BOOT** da placa.

Se a compilação disser *"Falta o arquivo secrets.h"*, rode o `setup.bat` ou copie o `secrets.h.example`.

## Wi-Fi

O Wi-Fi não fica no código. Na primeira vez, o LED fica **azul** e o ESP32 cria a rede
**`AlertaSons-Setup`**: conecte o celular nela, escolha a rede (ex.: o hotspot do celular) e a senha.
Fica salvo. Para trocar de rede, envie `w` pelo Serial Monitor.

## Comandos do Serial Monitor (115200)

| Comando | O que faz |
|---|---|
| `s 1` / `s 0` | Liga/desliga o envio ao Supabase (volta ao padrão de `secrets.h` ao reiniciar) |
| `l 1` / `l 0` | Mostra/oculta as probabilidades em tempo real |
| `w` | Esquece o Wi-Fi e abre o portal |
| `m` | Monitora o nível do microfone |
| `c N`, `p N`, `r N` | Gravam N amostras de campainha, palma ou ruído e mandam pelo USB (para o treino) |
| `?` | Ajuda |

## Cores do LED

| Cor | Significado |
|---|---|
| Azul | Esperando configurar o Wi-Fi |
| Vermelho | Palma detectada |
| Amarelo | Campainha detectada |
| Ciano | Evento enviado com sucesso |
| Rosa | Falha ao enviar |

## Ajustar a sensibilidade

`CONFIANCA_MIN` é a certeza mínima para disparar. Muitos falsos alertas: **suba** (0.97). Perdendo
sons reais: **desça** (0.90). Use `l 1` para ver o que a IA enxerga e decida com base nisso.

## Atualizar a IA

Depois de retreinar, o `exportar.py` copia sozinho `modelo_tflite.h` e `config_features.h` para esta
pasta. Basta gravar de novo no ESP32. Guia completo em [4-ia/GUIA-IA.md](../4-ia/GUIA-IA.md).

Se o modelo crescer (mais classes ou filtros), confira no Serial a linha *"arena usada X de Y bytes"* e
aumente `IA_TAMANHO_ARENA` em `ia_sons.h` se faltar.
