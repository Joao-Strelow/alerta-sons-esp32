"""
verificar_firmware.py  -  CONFERE SE O CÁLCULO DO ESP32 BATE COM O DO TREINO
============================================================================

O ESP32 calcula o espectrograma com um código C++ escrito à mão (ia_sons.h),
enquanto o treino usa numpy. Se houver qualquer diferença, a IA erra no
aparelho mesmo estando ótima no computador.

Este script REPRODUZ, passo a passo e em float32 (como no ESP32), o mesmo
algoritmo do firmware (FFT radix-2 com permutação bit-reversa, tabelas de
cosseno/seno, janela de Hann, filtros mel, logaritmo, normalização e
quantização int8) e compara o resultado com treino/features.py.

Como executar:
    .venv\\Scripts\\python.exe treino\\verificar_firmware.py

Resultado esperado: diferença máxima muito pequena (quantização int8 praticamente
idêntica) e as mesmas previsões do modelo.
"""

import json
import os

os.environ["TF_CPP_MIN_LOG_LEVEL"] = "3"

import numpy as np
import tensorflow as tf

import config
import dados
import features


# ---------------------------------------------------------------------------
# Reprodução do firmware (ia_sons.h) em Python, em float32
# ---------------------------------------------------------------------------
N = config.JANELA_FFT
TABELA_COS = np.cos(2 * np.pi * np.arange(N // 2) / N).astype(np.float32)
TABELA_SIN = np.sin(2 * np.pi * np.arange(N // 2) / N).astype(np.float32)


def tabela_bitrev():
    """Mesma lógica de ia_iniciar(): inverte a ordem dos bits de cada índice."""
    bits = int(np.log2(N))
    saida = np.zeros(N, dtype=np.int64)
    for i in range(N):
        r = 0
        for b in range(bits):
            if i & (1 << b):
                r |= 1 << (bits - 1 - b)
        saida[i] = r
    return saida


BITREV = tabela_bitrev()


def fft_firmware(re, im):
    """Mesmo algoritmo de ia_fft() no C++ (radix-2, in-place, float32)."""
    re, im = re.astype(np.float32).copy(), im.astype(np.float32).copy()
    for i in range(N):                      # 1) permutação bit-reversa
        j = BITREV[i]
        if j > i:
            re[i], re[j] = re[j], re[i]
            im[i], im[j] = im[j], im[i]
    tam = 2
    while tam <= N:                         # 2) borboletas
        meio, passo = tam >> 1, N // tam
        for i in range(0, N, tam):
            for k in range(meio):
                wr = TABELA_COS[k * passo]
                wi = -TABELA_SIN[k * passo]
                a, b = i + k, i + k + meio
                tr = np.float32(re[b] * wr - im[b] * wi)
                ti = np.float32(re[b] * wi + im[b] * wr)
                re[b], im[b] = re[a] - tr, im[a] - ti
                re[a], im[a] = re[a] + tr, im[a] + ti
        tam <<= 1
    return re, im


def entrada_firmware(audio_int16, media, desvio, escala, zero):
    """Replica ia_preencher_entrada(): devolve o tensor int8 (49, 32) do firmware."""
    saida = np.zeros((config.N_QUADROS, config.BANDAS_MEL), dtype=np.int8)
    hann = features.JANELA_HANN
    for q in range(config.N_QUADROS):
        pos = q * config.SALTO
        re = (audio_int16[pos:pos + N] / np.float32(32768.0)).astype(np.float32) * hann
        im = np.zeros(N, dtype=np.float32)
        re, im = fft_firmware(re, im)
        pot = (re[:config.JANELA_FFT // 2 + 1] ** 2 + im[:config.JANELA_FFT // 2 + 1] ** 2).astype(np.float32)
        mel = features.FILTROS_MEL @ pot
        x = np.log(mel + np.float32(config.EPSILON)).astype(np.float32)
        x = (x - np.float32(media)) / np.float32(desvio)
        v = np.floor(np.abs(x / escala) + 0.5) * np.sign(x / escala) + zero     # lroundf do C++
        saida[q] = np.clip(v, -128, 127).astype(np.int8)
    return saida


def main():
    norm = json.load(open(config.PASTA_MODELO / "normalizacao.json"))
    interp = tf.lite.Interpreter(model_path=str(config.PASTA_MODELO / "modelo_alerta.tflite"))
    interp.allocate_tensors()
    ent, sai = interp.get_input_details()[0], interp.get_output_details()[0]
    escala, zero = ent["quantization"]

    # Usa alguns áudios reais da pasta de teste (inteiros de 16 bits, como no ESP32)
    from scipy.io import wavfile
    arquivos = sorted(config.PASTA_TESTE.glob("*.wav"))[::8][:10]

    maior_dif, mesma_classe = 0, 0
    for arq in arquivos:
        _, pcm = wavfile.read(arq)
        audio = pcm.astype(np.float32) / 32768.0

        # (a) caminho do treino (numpy) -> quantiza como o exportar.py faz
        f = (features.extrair_features(audio) - norm["media"]) / norm["desvio"]
        q_treino = np.clip(np.round(f / escala + zero), -128, 127).astype(np.int8)

        # (b) caminho do firmware (C++ reproduzido)
        q_firmware = entrada_firmware(pcm, norm["media"], norm["desvio"], escala, zero)

        dif = int(np.abs(q_treino.astype(int) - q_firmware.astype(int)).max())
        maior_dif = max(maior_dif, dif)

        # previsões do modelo nos dois casos
        def prever(q):
            interp.set_tensor(ent["index"], q.reshape(1, config.N_QUADROS, config.BANDAS_MEL, 1))
            interp.invoke()
            return int(interp.get_tensor(sai["index"]).argmax())
        mesma_classe += int(prever(q_treino) == prever(q_firmware))
        print(f"{arq.name[:28]:<30} maior diferença int8: {dif}")

    print(f"\nMaior diferença (em unidades int8, escala -128..127): {maior_dif}")
    print(f"Mesma classe prevista pelos dois caminhos: {mesma_classe} de {len(arquivos)}")
    print("OK: o cálculo do firmware bate com o do treino." if maior_dif <= 1 and mesma_classe == len(arquivos)
          else "ATENÇÃO: há diferença relevante, revisar ia_sons.h.")


if __name__ == "__main__":
    main()
