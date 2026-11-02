# IA do Alerta de Sons (treino da rede neural)

Parte do projeto que cria a IA. O dispositivo **ESP32** escuta o ambiente por um microfone e, usando uma
**rede neural treinada por nós**, reconhece sons importantes (campainha, palmas) para avisar
pessoas com deficiência auditiva. Os alertas vão para o celular (Telegram) através de um
servidor na nuvem (Supabase).

## Estrutura desta pasta

```
ia/
├── dados/            Áudios gravados (1 s, 16 kHz, mono)
│   ├── training/       usados para o modelo APRENDER
│   └── testing/        nunca vistos no treino; só para AVALIAR
├── treino/           Código Python que cria e treina a IA
│   ├── config.py             parâmetros do projeto (um só lugar)
│   ├── features.py           áudio -> espectrograma mel ("imagem" do som)
│   ├── dados.py              leitura dos .wav e aumento de dados
│   ├── treinar.py            cria, treina e avalia a rede neural
│   ├── exportar.py           reduz o modelo (int8) e gera os .h para o ESP32
│   ├── verificar_firmware.py confere se o cálculo do ESP32 bate com o do treino
│   └── coletar_serial.py     grava novos sons pelo ESP32 (cabo USB) e salva em dados/
├── modelo/           Resultados: modelo, gráficos, relatórios e arquivos .h
└── requirements.txt  Bibliotecas Python necessárias
```

Os arquivos `modelo_tflite.h` e `config_features.h` gerados em `modelo/` são copiados para
`../firmware/alerta-sons-ia/`.
## Como a IA funciona (resumo para apresentação)

```
 microfone ─► 1 s de áudio ─► espectrograma mel ─► rede neural CNN ─► probabilidades
 INMP441      16.000 números   imagem 49 x 32       ~8 mil pesos      campainha 2%
                               (tempo x frequência)  aprendidos        palma    95%
                                                                        ruído     3%
```

1. **Coleta:** o próprio ESP32 grava trechos de 1 segundo de cada som (campainha, palma, ruído).
2. **Espectrograma mel** (`features.py`): a rede não analisa o áudio bruto. Transformamos o som numa
   "imagem" que mostra quais frequências existem em cada instante. A escala *mel* imita o ouvido humano.
3. **Aumento de dados** (`dados.py`): como temos poucos áudios, criamos variações (deslocamento no
   tempo, volume diferente, mistura com ruído de fundo) e igualamos a quantidade de cada classe.
4. **Rede neural convolucional (CNN)** (`treinar.py`): trata o espectrograma como imagem e aprende,
   sozinha, os padrões que diferenciam cada som.
5. **Avaliação:** testamos com áudios que a rede **nunca viu** (pasta `testing`). A *matriz de confusão*
   mostra onde ela acerta e onde confunde.
6. **Quantização int8** (`exportar.py`): o modelo é reduzido para números inteiros de 8 bits, ficando
   pequeno e rápido o bastante para rodar no microcontrolador, sem depender de internet para reconhecer.
7. **No ESP32:** o firmware calcula o mesmo espectrograma, roda o modelo (TensorFlow Lite Micro) e,
   ao detectar um som, pisca o LED e envia o evento ao servidor.

## Como executar o treino

Dentro da pasta `ia` (PowerShell):

```powershell
# 1) uma vez só: criar o ambiente e instalar as bibliotecas
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r requirements.txt

# 2) treinar a IA (gera modelo, gráficos e relatório em modelo/)
.\.venv\Scripts\python.exe treino\treinar.py

# 3) reduzir o modelo e gerar os arquivos .h para o ESP32
.\.venv\Scripts\python.exe treino\exportar.py
```

## Glossário (para a apresentação)

| Termo | Significado |
|---|---|
| **Espectrograma** | Gráfico de quanta energia há em cada frequência ao longo do tempo |
| **FFT** | Cálculo que separa um som nas frequências que o compõem |
| **Escala mel** | Escala de frequências que imita a percepção do ouvido humano |
| **CNN** | Rede neural com camadas "convolucionais", boas em reconhecer padrões em imagens |
| **Época** | Uma passada completa da rede por todos os exemplos de treino |
| **Overfitting** | Quando a rede "decora" os exemplos e erra em sons novos |
| **Data augmentation** | Criar variações dos dados para a rede generalizar melhor |
| **Matriz de confusão** | Tabela que mostra, para cada som real, o que a IA respondeu |
| **Quantização** | Reduzir a precisão dos números (32 bits -> 8 bits) para o modelo caber no ESP32 |
| **TensorFlow Lite Micro** | Versão do TensorFlow que roda em microcontroladores |

## Melhorar o modelo

1. Feche o Serial Monitor e grave sons reais: `python treino\coletar_serial.py COM3 ruido 40`
   (também `palma` e `campainha`). Variar o ambiente ajuda: sala quieta, TV, o aparelho no bolso.
2. Rode `treinar.py` e depois `exportar.py`.
3. Copie `modelo/modelo_tflite.h` e `modelo/config_features.h` para `../firmware/alerta-sons-ia/`
   e grave de novo no ESP32.

## Adicionar um novo tipo de som (ex.: sirene)

O tamanho do modelo quase não muda: cada classe nova acrescenta só ~768 pesos na última
camada. O modelo continua com poucos KB e cabe com folga no ESP32.

1. **Gravar** (Serial Monitor fechado), de 40 a 50 gravações em locais e distâncias diferentes:
   `python treino\coletar_serial.py COM3 sirene 40`
2. **Declarar a classe** em `treino/config.py`: acrescente `"sirene"` na lista `CLASSES`
   (mantenha `"ruido"` na lista; o código usa o nome para tratar o ruído).
3. **Treinar e exportar:** `python treino\treinar.py` e depois `python treino\exportar.py`.
   Veja `modelo/matriz_confusao.png` para saber quais sons a IA confunde. Se confundir muito,
   grave mais exemplos ou aumente as camadas da rede em `treinar.py`.
4. **Copiar** `modelo/modelo_tflite.h` e `modelo/config_features.h` para `../firmware/alerta-sons-ia/`.
5. **Ajustar o firmware** (`alerta-sons-ia.ino`): onde `palma` e `campainha` são tratadas
   (cor do LED e texto do evento enviado), inclua a nova classe.
6. **Ajustar o site e o Telegram:** `app/app.vue` (ícone e texto do alerta) e
   `supabase/functions/notificar-telegram/index.ts` (texto da mensagem).
   No Supabase nada muda: a coluna `tipo` da tabela `eventos` aceita qualquer texto.
7. **Regravar** o firmware no ESP32 e testar com `l 1` no Serial Monitor.
## Resultado atual

Teste com áudios nunca vistos: 92,5% de acerto (modelo float) e 90,0% (int8, o que roda no ESP32).

