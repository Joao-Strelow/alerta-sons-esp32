"""
dados.py  -  CARREGA OS ÁUDIOS E FAZ O "AUMENTO DE DADOS"
==========================================================

Duas tarefas:

  1. Ler os arquivos .wav das pastas dados/training e dados/testing.
     O nome do arquivo começa com a classe:  palma.12345.wav -> "palma".

  2. Data augmentation (aumento de dados): temos poucos áudios (cerca de 50
     palmas e 60 campainhas). Redes neurais precisam de MUITOS exemplos, então
     criamos variações de cada áudio: deslocado no tempo, mais alto/baixo, e
     misturado com ruído de fundo. O modelo aprende que "a palma continua sendo
     palma mesmo com barulho ou em outro momento do segundo".
"""

import numpy as np
from scipy.io import wavfile

import config


# ---------------------------------------------------------------------------
# LEITURA DOS ARQUIVOS
# ---------------------------------------------------------------------------
def carregar_pasta(pasta):
    """
    Lê todos os .wav de uma pasta.

    Devolve:
      audios  -> matriz (N, 16000) com valores entre -1.0 e +1.0
      rotulos -> vetor (N,) com o número da classe (0=campainha, 1=palma, 2=ruido)
      nomes   -> lista com o nome dos arquivos (útil para depurar)
    """
    audios, rotulos, nomes = [], [], []
    for arquivo in sorted(pasta.glob("*.wav")):
        classe = arquivo.name.split(".")[0]          # texto antes do primeiro ponto
        if classe not in config.CLASSES:
            continue                                 # ignora arquivos de outras classes

        taxa, dados = wavfile.read(arquivo)          # dados = números inteiros de 16 bits
        assert taxa == config.TAXA_AMOSTRAGEM, f"{arquivo.name}: taxa inesperada {taxa}"
        assert dados.shape[0] == config.AMOSTRAS_POR_CLIPE, f"{arquivo.name}: tamanho inesperado"

        # Converte de inteiro 16 bits (-32768..32767) para decimal (-1.0..+1.0).
        audios.append(dados.astype(np.float32) / 32768.0)
        rotulos.append(config.CLASSES.index(classe))
        nomes.append(arquivo.name)

    return np.stack(audios), np.array(rotulos, dtype=np.int64), nomes


# ---------------------------------------------------------------------------
# SEPARAÇÃO TREINO / VALIDAÇÃO
# ---------------------------------------------------------------------------
def separar_treino_validacao(audios, rotulos, fracao, rng):
    """
    Separa uma parte dos áudios (fracao) para VALIDAÇÃO.

    - Treino:    o modelo aprende com estes.
    - Validação: usados durante o treino para saber se ele está realmente
                 aprendendo ou só "decorando" (overfitting).
    - Teste:     pasta separada, só usada no FINAL.

    A separação é feita POR CLASSE, para manter a proporção de cada uma.
    É feita ANTES do aumento de dados, para que nenhuma variação de um áudio
    de treino "vaze" para a validação (isso daria um resultado falsamente bom).
    """
    indices_treino, indices_validacao = [], []
    for classe in range(len(config.CLASSES)):
        indices = np.where(rotulos == classe)[0]
        rng.shuffle(indices)                                   # embaralha
        n_val = max(1, int(round(len(indices) * fracao)))      # quantos vão para validação
        indices_validacao.extend(indices[:n_val])
        indices_treino.extend(indices[n_val:])
    return (audios[indices_treino], rotulos[indices_treino],
            audios[indices_validacao], rotulos[indices_validacao])


# ---------------------------------------------------------------------------
# AUMENTO DE DADOS
# ---------------------------------------------------------------------------
def variar_audio(audio, banco_ruidos, rng, ganho=(0.4, 1.6), escala_log=False, prob_mistura=0.7):
    """
    Cria UMA variação aleatória de um áudio. Três transformações:

      1. Deslocamento no tempo (até ±0,2 s): o som pode acontecer em qualquer
         momento da janela de 1 s, não só no meio.
      2. Ganho (volume): a palma pode ser forte ou fraca, perto ou longe do
         microfone. Por padrão entre 0,4x e 1,6x.
      3. Mistura de ruído de fundo: soma um trecho da classe "ruido" (com
         volume aleatório). Na rua/casa os sons nunca estão isolados.

    Parâmetros opcionais (usados na classe "ruido", ver aumentar_e_balancear):
      ganho        - faixa (mínimo, máximo) do ganho de volume
      escala_log   - True = sorteia o ganho em escala logarítmica, o que
                     distribui melhor volumes muito baixos e muito altos
      prob_mistura - probabilidade de somar outro ruído de fundo
    """
    # 1) deslocamento: desloca o vetor e preenche o espaço vazio com silêncio (zeros)
    deslocamento = int(rng.integers(-3200, 3201))
    novo = np.zeros_like(audio)
    if deslocamento >= 0:
        novo[deslocamento:] = audio[:len(audio) - deslocamento]
    else:
        novo[:deslocamento] = audio[-deslocamento:]

    # 2) ganho (volume)
    if escala_log:
        fator = np.exp(rng.uniform(np.log(ganho[0]), np.log(ganho[1])))
    else:
        fator = rng.uniform(ganho[0], ganho[1])
    novo = novo * fator

    # 3) ruído de fundo (por padrão em 70% das variações)
    if rng.random() < prob_mistura:
        ruido = banco_ruidos[rng.integers(len(banco_ruidos))]
        novo = novo + ruido * rng.uniform(0.5, 3.0)

    # Garante que nada passe do limite do áudio digital (-1.0 a +1.0).
    return np.clip(novo, -1.0, 1.0).astype(np.float32)


def ruido_sintetico(rng):
    """
    Cria 1 segundo de ruído ARTIFICIAL de volume BAIXO (de quase silêncio até
    rms ~150 em escala de 16 bits).

    Por que isto existe? Todos os áudios de "ruido" gravados foram em ambientes
    relativamente barulhentos (rms mediano ~440). A IA nunca tinha "visto" uma
    sala quieta e, ao ouvir uma, respondia campainha/palma com alta confiança
    (falso alarme no silêncio). Estes ruídos artificiais ensinam que
    "som muito fraco = ruído".

    O ruído é uma mistura de ruído branco (todas as frequências) com uma versão
    suavizada (mais graves), para variar o "timbre" do fundo.
    """
    n = config.AMOSTRAS_POR_CLIPE

    # 5% dos casos: silêncio digital quase perfeito
    if rng.random() < 0.05:
        return np.zeros(n, dtype=np.float32)

    # volume (rms) sorteado em escala logarítmica entre 1 e 150 (de 32768)
    rms = np.exp(rng.uniform(np.log(1.0), np.log(150.0))) / 32768.0

    branco = rng.normal(0.0, 1.0, n)
    grave = np.convolve(branco, np.ones(8) / 8.0, mode="same")   # média móvel = filtro passa-baixa
    proporcao = rng.uniform(0.0, 1.0)                             # quanto de "grave" misturar
    ruido = (1.0 - proporcao) * branco + proporcao * grave
    ruido = ruido / (ruido.std() + 1e-9) * rms                    # ajusta para o volume sorteado

    # O microfone real entrega números inteiros de 16 bits: arredonda igual.
    return (np.round(ruido * 32768.0) / 32768.0).astype(np.float32)


def aumentar_e_balancear(audios, rotulos, rng):
    """
    Gera o conjunto de treino final com o MESMO número de exemplos por classe
    (config.AMOSTRAS_POR_CLASSE).

    Por que balancear? Temos ~130 áudios de ruído e só ~45 de palma. Se
    treinássemos assim, a IA aprenderia que "chutar ruído" acerta quase sempre.
    Igualando as quantidades, ela precisa realmente distinguir as classes.

    O ruído usado nas misturas vem SÓ dos áudios de treino (nunca da validação
    nem do teste).
    """
    indice_ruido = config.CLASSES.index("ruido")
    banco_ruidos = audios[rotulos == indice_ruido]

    saida_audios, saida_rotulos = [], []
    for classe in range(len(config.CLASSES)):
        originais = audios[rotulos == classe]
        for i in range(config.AMOSTRAS_POR_CLASSE):
            # Percorre os originais em rodízio e cria uma variação de cada vez.
            base = originais[i % len(originais)]
            # A primeira passada mantém o original; depois só variações.
            if i < len(originais):
                saida_audios.append(base)
            elif classe == indice_ruido and rng.random() < 0.25:
                # 25% do ruído extra é artificial e de volume baixo (sala quieta/silêncio)
                saida_audios.append(ruido_sintetico(rng))
            elif classe == indice_ruido:
                # Demais variações do ruído: volume de 0,03x a 2x (inclui ambientes
                # bem mais quietos que os gravados) e pouca mistura de outro ruído.
                saida_audios.append(variar_audio(base, banco_ruidos, rng,
                                                 ganho=(0.03, 2.0), escala_log=True,
                                                 prob_mistura=0.3))
            else:
                saida_audios.append(variar_audio(base, banco_ruidos, rng))
            saida_rotulos.append(classe)

    saida_audios = np.stack(saida_audios)
    saida_rotulos = np.array(saida_rotulos, dtype=np.int64)

    # Embaralha para que as classes não fiquem em blocos.
    ordem = rng.permutation(len(saida_rotulos))
    return saida_audios[ordem], saida_rotulos[ordem]
