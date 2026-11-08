# Diagramas do projeto Alerta de Sons

Os diagramas usam Mermaid (renderizam no GitHub e no VS Code com a extensão
"Markdown Preview Mermaid Support").

## 1. Coleta de sons e treino da IA (acontece no PC)

```mermaid
flowchart TD
    subgraph ESP[ESP32 conectado por USB]
        MIC1[Microfone INMP441] --> GRAV["Comando c / p / r N<br/>grava 1 s de áudio"]
    end
    GRAV -- "cabo USB (serial)" --> COL[coletar_serial.py<br/>salva arquivos .wav]
    COL -- "80% das gravações" --> TR[(4-ia/dados/training)]
    COL -- "20% das gravações" --> TE[(4-ia/dados/testing)]
    TR --> TREINAR
    TE --> TREINAR
    subgraph PC[Treino no computador - pasta 4-ia/treino/]
        TREINAR["treinar.py<br/>1. aumenta e balanceia os dados<br/>2. espectrograma mel (features.py)<br/>3. treina a CNN<br/>4. avalia no conjunto de teste"]
        TREINAR --> MOD[(modelo/<br/>modelo_keras.keras<br/>gráficos e relatório)]
        MOD --> EXP["exportar.py<br/>quantiza para int8 (~14 KB)<br/>gera arquivos .h"]
        EXP --> H["modelo_tflite.h<br/>config_features.h"]
        H --> VER["verificar_firmware.py<br/>confere: cálculo do ESP32 = cálculo do treino"]
    end
    H -- "copiados para 3-aparelho/alerta-sons-ia/" --> IDE[Arduino IDE<br/>compila e grava]
    IDE --> ESP2[ESP32 com a IA nova]
```

## 2. Funcionamento do ESP32 (operação normal)

```mermaid
flowchart TD
    MIC[Microfone INMP441] -- "I2S, 16 kHz" --> CAP["Tarefa Captura<br/>escreve no anel de áudio"]
    CAP --> ANEL[("Anel de áudio<br/>último 1 s<br/>nova análise a cada 250 ms")]
    ANEL --> IA["ia_sons.h<br/>1. espectrograma mel 49x32<br/>2. rede neural CNN (TensorFlow Lite Micro)"]
    IA --> PROB["Probabilidades<br/>campainha / palma / ruído"]
    PROB --> DET{"Palma ou campainha?<br/>confiança >= 95%?<br/>passaram 5 s do último?"}
    DET -- "não" --> ESC[Continua escutando]
    DET -- "sim" --> FILA[Fila de eventos]
    FILA --> ACAO["Tarefa Ações<br/>LED: vermelho = palma, amarelo = campainha"]
    ACAO --> ENV{"Envio ao Supabase<br/>ligado? (s 1)"}
    ENV -- "sim" --> POST["POST /rest/v1/eventos<br/>node_id, tipo, intensidade"]
    HB["Tarefa Heartbeat<br/>a cada 30 s"] --> POST2["POST /rest/v1/nodes<br/>last_seen = agora"]
    WM["WiFiManager<br/>rede AlertaSons-Setup<br/>configura o Wi-Fi do lugar"] -.-> POST
    WM -.-> POST2
```

## 3. Da detecção ao alerta no celular e na tela (nuvem)

```mermaid
flowchart LR
    ESP[ESP32] -- "HTTPS: evento" --> DB[("Supabase<br/>Postgres<br/>tabelas eventos, nodes, config")]
    ESP -- "HTTPS: heartbeat" --> DB
    DB -- "Realtime (WebSocket)" --> WEB["Site Nuxt (PWA)<br/>alerta gigante em tela<br/>histórico e status online/offline"]
    DB -- "Database Webhook<br/>INSERT em eventos" --> FN["Edge Function<br/>notificar-telegram"]
    FN -- "lê telegram_chat_id" --> DB
    FN -- "sendMessage" --> TG[Bot do Telegram]
    TG --> CEL[Celular da pessoa]
    WEB -- "pessoa cola o chat ID" --> DB
```

## 4. Visão geral (tudo junto)

```mermaid
flowchart LR
    SOM(("Som do<br/>ambiente")) --> ESP["ESP32 + microfone<br/>IA própria no aparelho"]
    ESP -- "só o evento<br/>(nenhum áudio é enviado)" --> SB[("Supabase")]
    SB --> SITE[Site em tempo real]
    SB --> BOT[Telegram]
    SITE --> PESSOA(("Pessoa<br/>com deficiência<br/>auditiva"))
    BOT --> PESSOA
    PC["PC: coleta, treino<br/>e exportação da IA"] -. "modelo treinado<br/>(gravado no ESP32 via USB)" .-> ESP
```
