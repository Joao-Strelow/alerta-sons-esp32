"""
treinar.py  -  TREINA A REDE NEURAL
====================================

Como executar (na pasta do projeto):

    .venv\\Scripts\\python.exe treino\\treinar.py

O que este script faz, em ordem:

  1. Carrega os áudios (treino e teste).
  2. Separa uma parte do treino para validação.
  3. Aumenta e balanceia os dados de treino (data augmentation).
  4. Converte todos os áudios em espectrogramas mel (a "imagem" do som).
  5. Normaliza os números (média 0, desvio 1) - ajuda a rede a aprender.
  6. Cria e treina a rede neural convolucional (CNN).
  7. Avalia no conjunto de TESTE (áudios que a rede nunca viu).
  8. Salva modelo, gráficos e relatório na pasta "modelo/".
"""

import json
import os

os.environ["TF_CPP_MIN_LOG_LEVEL"] = "2"      # esconde mensagens técnicas do TensorFlow

import matplotlib
matplotlib.use("Agg")                          # gera imagens sem abrir janelas
import matplotlib.pyplot as plt
import numpy as np
import tensorflow as tf

import config
import dados
import features


# ---------------------------------------------------------------------------
# A REDE NEURAL
# ---------------------------------------------------------------------------
def criar_modelo():
    """
    Rede neural convolucional (CNN) pequena, pensada para caber no ESP32.

    Ideia: tratamos o espectrograma (49 x 32) como uma imagem pequena. As
    camadas "Conv2D" funcionam como detectores de padrões visuais (por
    exemplo, "um estouro curto em todas as frequências" = palma; "faixas
    horizontais em frequências específicas" = campainha). Cada bloco
    Conv2D + MaxPooling detecta padrões e reduz o tamanho da imagem.

    As camadas usadas são todas compatíveis com o TensorFlow Lite Micro
    (a versão do TensorFlow que roda em microcontroladores).
    """
    return tf.keras.Sequential([
        # Entrada: espectrograma 49 x 32 com 1 canal (como uma foto em preto e branco).
        tf.keras.layers.Input(shape=config.FORMATO_ENTRADA),

        # Bloco 1: 8 detectores de padrões 3x3 + reduz a imagem pela metade.
        tf.keras.layers.Conv2D(8, 3, padding="same", activation="relu"),
        tf.keras.layers.MaxPooling2D(2),            # 49x32 -> 24x16

        # Bloco 2: 16 detectores de padrões mais complexos.
        tf.keras.layers.Conv2D(16, 3, padding="same", activation="relu"),
        tf.keras.layers.MaxPooling2D(2),            # 24x16 -> 12x8

        # Bloco 3: 32 detectores ainda mais abstratos.
        tf.keras.layers.Conv2D(32, 3, padding="same", activation="relu"),
        tf.keras.layers.MaxPooling2D(2),            # 12x8 -> 6x4

        # "Estica" a imagem 6x4x32 num vetor de 768 números.
        # Usamos Reshape com tamanho fixo (e não Flatten) porque assim o modelo
        # convertido fica com UMA operação simples, fácil para o ESP32 executar.
        tf.keras.layers.Reshape((6 * 4 * 32,)),

        # Dropout: durante o treino "desliga" 30% dos neurônios aleatoriamente.
        # Evita que a rede decore os exemplos (overfitting).
        tf.keras.layers.Dropout(0.3),

        # Camada final: 3 saídas (campainha, palma, ruído). "softmax" transforma
        # os números em probabilidades que somam 100%.
        tf.keras.layers.Dense(len(config.CLASSES), activation="softmax"),
    ])


# ---------------------------------------------------------------------------
# MÉTRICAS
# ---------------------------------------------------------------------------
def matriz_confusao(verdadeiro, previsto):
    """
    Matriz de confusão: linha = classe REAL, coluna = classe que a IA previu.
    A diagonal principal são os acertos. Fora dela, os erros.
    """
    n = len(config.CLASSES)
    m = np.zeros((n, n), dtype=int)
    for v, p in zip(verdadeiro, previsto):
        m[v, p] += 1
    return m


def texto_relatorio(titulo, verdadeiro, previsto):
    """Monta um texto com acurácia geral e precisão/revocação de cada classe."""
    m = matriz_confusao(verdadeiro, previsto)
    linhas = [f"=== {titulo} ===",
              f"Acurácia geral: {np.trace(m) / m.sum() * 100:.1f}%  ({np.trace(m)} de {m.sum()} corretos)",
              "",
              "Matriz de confusão (linhas = real, colunas = previsto):",
              "            " + "  ".join(f"{c[:9]:>9}" for c in config.CLASSES)]
    for i, c in enumerate(config.CLASSES):
        linhas.append(f"{c:>11} " + "  ".join(f"{x:>9}" for x in m[i]))
    linhas.append("")
    for i, c in enumerate(config.CLASSES):
        # Revocação (recall): dos sons que ERAM desta classe, quantos a IA achou?
        revocacao = m[i, i] / max(1, m[i].sum())
        # Precisão: quando a IA DISSE esta classe, quantas vezes estava certa?
        precisao = m[i, i] / max(1, m[:, i].sum())
        linhas.append(f"{c:>10}: revocação {revocacao * 100:5.1f}% | precisão {precisao * 100:5.1f}%")
    return "\n".join(linhas), m


def salvar_grafico_confusao(m, caminho):
    """Desenha a matriz de confusão como imagem (para o relatório/apresentação)."""
    fig, ax = plt.subplots(figsize=(5, 4))
    ax.imshow(m, cmap="Blues")
    ax.set_xticks(range(len(config.CLASSES)), config.CLASSES)
    ax.set_yticks(range(len(config.CLASSES)), config.CLASSES)
    ax.set_xlabel("Previsto pela IA")
    ax.set_ylabel("Real")
    ax.set_title("Matriz de confusão (conjunto de teste)")
    for i in range(m.shape[0]):
        for j in range(m.shape[1]):
            ax.text(j, i, m[i, j], ha="center", va="center",
                    color="white" if m[i, j] > m.max() / 2 else "black")
    fig.tight_layout()
    fig.savefig(caminho, dpi=120)
    plt.close(fig)


def salvar_grafico_historico(historico, caminho):
    """Desenha como a acurácia e o erro (loss) evoluíram a cada época."""
    fig, eixos = plt.subplots(1, 2, figsize=(10, 4))
    eixos[0].plot(historico.history["accuracy"], label="treino")
    eixos[0].plot(historico.history["val_accuracy"], label="validação")
    eixos[0].set_title("Acurácia por época")
    eixos[0].set_xlabel("época")
    eixos[0].legend()
    eixos[1].plot(historico.history["loss"], label="treino")
    eixos[1].plot(historico.history["val_loss"], label="validação")
    eixos[1].set_title("Erro (loss) por época")
    eixos[1].set_xlabel("época")
    eixos[1].legend()
    fig.tight_layout()
    fig.savefig(caminho, dpi=120)
    plt.close(fig)


# ---------------------------------------------------------------------------
# PROGRAMA PRINCIPAL
# ---------------------------------------------------------------------------
def main():
    config.PASTA_MODELO.mkdir(exist_ok=True)
    rng = np.random.default_rng(config.SEMENTE)          # gerador de números aleatórios
    tf.keras.utils.set_random_seed(config.SEMENTE)       # idem para o TensorFlow

    # 1) Carregar áudios ------------------------------------------------------
    print("1) Carregando áudios...")
    audios_treino, rotulos_treino, _ = dados.carregar_pasta(config.PASTA_TREINO)
    audios_teste, rotulos_teste, _ = dados.carregar_pasta(config.PASTA_TESTE)
    for nome, r in (("treino", rotulos_treino), ("teste", rotulos_teste)):
        contagem = {c: int((r == i).sum()) for i, c in enumerate(config.CLASSES)}
        print(f"   {nome}: {contagem}")

    # 2) Separar validação ----------------------------------------------------
    audios_tr, rotulos_tr, audios_val, rotulos_val = dados.separar_treino_validacao(
        audios_treino, rotulos_treino, config.FRACAO_VALIDACAO, rng)
    print(f"2) Treino: {len(audios_tr)} áudios | Validação: {len(audios_val)} áudios")

    # 3) Aumentar e balancear o treino ---------------------------------------
    print("3) Aumentando e balanceando os dados de treino...")
    audios_tr, rotulos_tr = dados.aumentar_e_balancear(audios_tr, rotulos_tr, rng)
    print(f"   Treino após aumento: {len(audios_tr)} exemplos "
          f"({config.AMOSTRAS_POR_CLASSE} por classe)")

    # 4) Áudio -> espectrograma ----------------------------------------------
    print("4) Calculando espectrogramas mel...")
    x_tr = features.extrair_lote(audios_tr)
    x_val = features.extrair_lote(audios_val)
    x_teste = features.extrair_lote(audios_teste)

    # 5) Normalização ---------------------------------------------------------
    # Calculamos média e desvio padrão SÓ do treino e aplicamos aos três
    # conjuntos. Os MESMOS dois números serão usados no ESP32.
    media = float(x_tr.mean())
    desvio = float(x_tr.std())
    print(f"5) Normalização: média={media:.4f}  desvio={desvio:.4f}")

    def normalizar(x):
        # (x - média) / desvio e adiciona o eixo do "canal" (49,32) -> (49,32,1)
        return ((x - media) / desvio)[..., np.newaxis].astype(np.float32)

    x_tr, x_val, x_teste = normalizar(x_tr), normalizar(x_val), normalizar(x_teste)

    # 6) Criar e treinar a rede ----------------------------------------------
    print("6) Treinando a rede neural...")
    modelo = criar_modelo()
    modelo.summary()
    modelo.compile(
        optimizer=tf.keras.optimizers.Adam(config.TAXA_APRENDIZADO),   # algoritmo que ajusta os pesos
        loss="sparse_categorical_crossentropy",                        # mede o "erro" da rede
        metrics=["accuracy"],
    )
    historico = modelo.fit(
        x_tr, rotulos_tr,
        validation_data=(x_val, rotulos_val),
        epochs=config.EPOCAS_MAXIMAS,
        batch_size=config.TAMANHO_LOTE,
        callbacks=[
            # Para o treino quando a validação para de melhorar e volta
            # aos melhores pesos encontrados (evita overfitting).
            tf.keras.callbacks.EarlyStopping(monitor="val_loss", patience=10,
                                             restore_best_weights=True),
            # Reduz o "tamanho do passo" quando o aprendizado estagna.
            tf.keras.callbacks.ReduceLROnPlateau(monitor="val_loss", factor=0.5, patience=4),
        ],
        verbose=2,
    )

    # 7) Avaliação ------------------------------------------------------------
    print("7) Avaliando...")
    prev_val = modelo.predict(x_val, verbose=0).argmax(axis=1)
    prev_teste = modelo.predict(x_teste, verbose=0).argmax(axis=1)
    txt_val, _ = texto_relatorio("VALIDAÇÃO", rotulos_val, prev_val)
    txt_teste, m_teste = texto_relatorio("TESTE (áudios nunca vistos)", rotulos_teste, prev_teste)

    # Teste de volume baixo: pega os áudios de RUÍDO do teste e vai diminuindo o
    # volume. Numa sala quieta o aparelho ouve algo assim; a IA tem que continuar
    # respondendo "ruído". (Falhas aqui significam falsos alarmes no silêncio.)
    indice_ruido = config.CLASSES.index("ruido")
    ruidos_teste = audios_teste[rotulos_teste == indice_ruido]
    linhas_vol = ["=== TESTE DE VOLUME BAIXO (ruídos do teste com volume reduzido) ===",
                  "Percentual classificado corretamente como ruído:"]
    for fator in (1.0, 0.3, 0.1, 0.03):
        prev = modelo.predict(normalizar(features.extrair_lote(ruidos_teste * fator)),
                              verbose=0).argmax(axis=1)
        linhas_vol.append(f"  volume {fator:>4}x: {(prev == indice_ruido).mean() * 100:5.1f}%")
    silencio = [np.zeros(config.AMOSTRAS_POR_CLIPE, dtype=np.float32)] + \
               [dados.ruido_sintetico(np.random.default_rng(100 + k)) for k in range(30)]
    prev = modelo.predict(normalizar(features.extrair_lote(np.stack(silencio))),
                          verbose=0).argmax(axis=1)
    linhas_vol.append(f"  silêncio e ruído artificial fraco: {(prev == indice_ruido).mean() * 100:5.1f}%")
    txt_volume = "\n".join(linhas_vol)

    print("\n" + txt_val + "\n\n" + txt_teste + "\n\n" + txt_volume)

    # 8) Salvar ---------------------------------------------------------------
    modelo.save(config.PASTA_MODELO / "modelo_keras.keras")
    with open(config.PASTA_MODELO / "normalizacao.json", "w") as f:
        json.dump({"media": media, "desvio": desvio, "classes": config.CLASSES}, f, indent=2)
    # Dados que o exportar.py precisa: exemplos para calibrar a quantização
    # e o conjunto de teste para comparar modelo normal x modelo reduzido.
    np.savez_compressed(config.PASTA_MODELO / "dados_exportacao.npz",
                        representativos=x_tr[::5], x_teste=x_teste, y_teste=rotulos_teste)
    with open(config.PASTA_MODELO / "relatorio_treino.txt", "w", encoding="utf-8") as f:
        f.write(txt_val + "\n\n" + txt_teste + "\n\n" + txt_volume + "\n")
    salvar_grafico_historico(historico, config.PASTA_MODELO / "grafico_treino.png")
    salvar_grafico_confusao(m_teste, config.PASTA_MODELO / "matriz_confusao.png")
    print(f"\n8) Arquivos salvos em: {config.PASTA_MODELO}")


if __name__ == "__main__":
    main()
