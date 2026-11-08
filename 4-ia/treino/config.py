"""
config.py  -  PARÂMETROS DO PROJETO (um só lugar para tudo)
=============================================================

Este arquivo guarda todas as "configurações" usadas no treino da IA.
Os MESMOS valores precisam ser usados no ESP32 (firmware), senão o modelo
treinado aqui não reconhece nada no aparelho. Por isso eles ficam centralizados.

Visão geral do que o sistema faz (resumo para explicar ao professor):

    áudio (1 segundo) ──► espectrograma mel ──► rede neural (CNN) ──► classe
        microfone          "imagem" do som       aprende padrões      campainha /
                                                                      palma / ruído
"""

from pathlib import Path

# ---------------------------------------------------------------------------
# PASTAS
# ---------------------------------------------------------------------------
# Path(__file__) = este arquivo; .parent.parent = pasta raiz do projeto.
RAIZ = Path(__file__).resolve().parent.parent
PASTA_TREINO = RAIZ / "dados" / "training"   # áudios usados para o modelo APRENDER
PASTA_TESTE = RAIZ / "dados" / "testing"     # áudios nunca vistos, só para AVALIAR
PASTA_MODELO = RAIZ / "modelo"               # onde ficam os resultados (modelo, gráficos)
# Pasta do firmware do ESP32: o exportar.py copia para lá os dois arquivos .h gerados.
PASTA_FIRMWARE = RAIZ.parent / "3-aparelho" / "alerta-sons-ia"

# ---------------------------------------------------------------------------
# CLASSES (o que a IA precisa distinguir)
# ---------------------------------------------------------------------------
# A ORDEM IMPORTA: a IA devolve 3 números (probabilidades), e o primeiro
# corresponde a "campainha", o segundo a "palma" e o terceiro a "ruido".
# No ESP32 a mesma ordem é usada para saber qual classe ganhou.
CLASSES = ["campainha", "palma", "ruido"]

# ---------------------------------------------------------------------------
# ÁUDIO
# ---------------------------------------------------------------------------
TAXA_AMOSTRAGEM = 16000          # 16.000 medições do microfone por segundo (16 kHz)
AMOSTRAS_POR_CLIPE = 16000       # cada gravação tem 1 segundo = 16.000 amostras

# ---------------------------------------------------------------------------
# EXTRAÇÃO DE CARACTERÍSTICAS (espectrograma mel)
# ---------------------------------------------------------------------------
# A rede neural não "ouve" o áudio bruto. Primeiro transformamos o som numa
# espécie de imagem (tempo x frequência), chamada espectrograma.
#
# JANELA_FFT: tamanho de cada "pedacinho" de áudio analisado (512 amostras = 32 ms).
# SALTO:      de quantas em quantas amostras começamos um novo pedacinho
#             (320 amostras = 20 ms). Pedaços vizinhos se sobrepõem.
# BANDAS_MEL: em quantas faixas de frequência dividimos o som. A escala "mel"
#             imita o ouvido humano (distingue melhor graves do que agudos).
# FREQ_MIN/MAX: faixa de frequências consideradas (80 Hz a 8 kHz; 8 kHz é o
#               máximo possível com 16 kHz de taxa, por causa do teorema de Nyquist).
JANELA_FFT = 512
SALTO = 320
BANDAS_MEL = 32
FREQ_MIN = 80.0
FREQ_MAX = 8000.0

# Quantos pedacinhos (colunas de tempo) cabem em 1 segundo de áudio:
#   (16000 - 512) / 320 + 1 = 49
N_QUADROS = 1 + (AMOSTRAS_POR_CLIPE - JANELA_FFT) // SALTO

# Entrada da rede neural: imagem de N_QUADROS x BANDAS_MEL (49 x 32), 1 canal.
FORMATO_ENTRADA = (N_QUADROS, BANDAS_MEL, 1)

# Pequeno valor somado antes do logaritmo, para nunca calcular log(0).
EPSILON = 1e-6

# ---------------------------------------------------------------------------
# TREINAMENTO
# ---------------------------------------------------------------------------
SEMENTE = 42                     # fixa o "sorteio": todo mundo obtém o mesmo resultado
FRACAO_VALIDACAO = 0.15          # 15% dos áudios de treino ficam só para validar
AMOSTRAS_POR_CLASSE = 1500       # após o "aumento de dados", cada classe terá ~1500 exemplos
EPOCAS_MAXIMAS = 80              # no máximo 80 passadas completas pelos dados
TAMANHO_LOTE = 32                # a rede ajusta seus pesos a cada 32 exemplos
TAXA_APRENDIZADO = 1e-3          # "tamanho do passo" do ajuste dos pesos
