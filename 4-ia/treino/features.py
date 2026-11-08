"""
features.py  -  TRANSFORMA ÁUDIO EM "IMAGEM" (espectrograma mel logarítmico)
============================================================================

IMPORTANTE: o ESP32 precisa fazer EXATAMENTE a mesma conta que este arquivo.
Por isso, depois do treino, as tabelas usadas aqui (janela de Hann e filtros mel)
são exportadas para o firmware (ver exportar.py). Assim os números batem.

Passo a passo (para cada 1 segundo de áudio):

  1. Corta o áudio em pedacinhos de 32 ms, andando de 20 em 20 ms (49 pedaços).
  2. Multiplica cada pedaço por uma "janela de Hann" (suaviza as bordas,
     evitando erros na etapa seguinte).
  3. Aplica a FFT (Transformada Rápida de Fourier): descobre QUANTA energia
     existe em cada frequência naquele pedaço.
  4. Junta as 257 frequências da FFT em 32 faixas "mel" (imitam o ouvido humano).
  5. Aplica o logaritmo (o ouvido percebe volume em escala logarítmica e isso
     deixa sons fracos e fortes numa faixa de valores parecida).

Resultado: uma matriz 49 x 32 (tempo x frequência) = a "imagem" do som.
"""

import numpy as np

import config


# ---------------------------------------------------------------------------
# 1) JANELA DE HANN
# ---------------------------------------------------------------------------
def criar_janela_hann():
    """
    Janela de Hann "periódica" de JANELA_FFT pontos.

    Fórmula:  w[n] = 0.5 - 0.5 * cos(2*pi*n / N)

    Vale 0 nas pontas e 1 no meio. Evita "cortes bruscos" no pedaço de áudio,
    que gerariam frequências falsas na FFT.
    """
    n = np.arange(config.JANELA_FFT)
    return (0.5 - 0.5 * np.cos(2.0 * np.pi * n / config.JANELA_FFT)).astype(np.float32)


# ---------------------------------------------------------------------------
# 2) FILTROS MEL
# ---------------------------------------------------------------------------
def hz_para_mel(hz):
    """Converte frequência em Hz para a escala mel (fórmula HTK)."""
    return 2595.0 * np.log10(1.0 + hz / 700.0)


def mel_para_hz(mel):
    """Conversão inversa: de mel para Hz."""
    return 700.0 * (10.0 ** (mel / 2595.0) - 1.0)


def criar_filtros_mel():
    """
    Cria a matriz de filtros mel, de formato (BANDAS_MEL, 257).

    Cada linha é um filtro TRIANGULAR: pega as frequências vizinhas de um
    ponto central e soma, dando mais peso ao centro. Os triângulos são
    espaçados igualmente na escala mel (logo, mais "apertados" nos graves).

    A FFT de 512 pontos produz 512/2 + 1 = 257 frequências (de 0 até 8 kHz).
    """
    n_bins = config.JANELA_FFT // 2 + 1                       # 257 frequências
    freqs_fft = np.linspace(0, config.TAXA_AMOSTRAGEM / 2, n_bins)  # Hz de cada bin

    # BANDAS_MEL + 2 pontos igualmente espaçados em mel (os extremos são as
    # pontas dos triângulos; os demais são os centros e as pontas vizinhas).
    mel_pontos = np.linspace(hz_para_mel(config.FREQ_MIN),
                             hz_para_mel(config.FREQ_MAX),
                             config.BANDAS_MEL + 2)
    hz_pontos = mel_para_hz(mel_pontos)

    filtros = np.zeros((config.BANDAS_MEL, n_bins), dtype=np.float32)
    for m in range(config.BANDAS_MEL):
        esquerda, centro, direita = hz_pontos[m], hz_pontos[m + 1], hz_pontos[m + 2]
        # lado que sobe (0 -> 1) entre "esquerda" e "centro"
        sobe = (freqs_fft - esquerda) / (centro - esquerda)
        # lado que desce (1 -> 0) entre "centro" e "direita"
        desce = (direita - freqs_fft) / (direita - centro)
        # o triângulo é o menor dos dois lados, nunca negativo
        filtros[m] = np.maximum(0.0, np.minimum(sobe, desce))
    return filtros


# As tabelas são calculadas UMA vez ao importar o arquivo (são sempre iguais).
JANELA_HANN = criar_janela_hann()
FILTROS_MEL = criar_filtros_mel()


# ---------------------------------------------------------------------------
# 3) FUNÇÃO PRINCIPAL: ÁUDIO -> ESPECTROGRAMA MEL LOGARÍTMICO
# ---------------------------------------------------------------------------
def extrair_features(audio):
    """
    Recebe 1 segundo de áudio (vetor de 16000 números entre -1.0 e +1.0)
    e devolve a matriz de características, formato (49, 32), tipo float32.
    """
    audio = np.asarray(audio, dtype=np.float32)
    assert audio.shape[0] == config.AMOSTRAS_POR_CLIPE, "o áudio precisa ter exatamente 1 segundo"

    quadros = []
    for i in range(config.N_QUADROS):
        inicio = i * config.SALTO
        pedaco = audio[inicio: inicio + config.JANELA_FFT]       # pedaço de 32 ms
        pedaco = pedaco * JANELA_HANN                            # passo 2: janela de Hann
        espectro = np.fft.rfft(pedaco)                           # passo 3: FFT (números complexos)
        potencia = espectro.real ** 2 + espectro.imag ** 2       # energia = parte real² + imaginária²
        quadros.append(potencia)

    potencia = np.array(quadros, dtype=np.float32)               # (49, 257)
    mel = potencia @ FILTROS_MEL.T                               # passo 4: (49,257)x(257,32) = (49,32)
    return np.log(mel + config.EPSILON).astype(np.float32)       # passo 5: logaritmo natural


def extrair_lote(audios):
    """Aplica extrair_features em vários áudios. Devolve (N, 49, 32)."""
    return np.stack([extrair_features(a) for a in audios])
