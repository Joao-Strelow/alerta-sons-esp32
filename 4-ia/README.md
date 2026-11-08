# 4 · IA (treino da rede neural) — opcional

**Para que serve:** cria o "cérebro" do aparelho: a rede neural que reconhece palma e campainha.
**O modelo já vem treinado** em `modelo/`. Esta pasta só é necessária para **retreinar com mais dados**
ou **ensinar um som novo** (como sirene).

> **Guia completo: [GUIA-IA.md](GUIA-IA.md)**, com o funcionamento do treino, o que cada parâmetro
> faz e o passo a passo para adicionar um som.

## O que tem aqui

| Pasta/arquivo | Função |
|---|---|
| `treino/` | Código Python: `config.py` (parâmetros), `features.py`, `dados.py`, `treinar.py`, `exportar.py`, `verificar_firmware.py`, `coletar_serial.py` |
| `dados/training/` | Áudios que a IA usa para **aprender** (302 arquivos) |
| `dados/testing/` | Áudios **nunca vistos** no treino, só para avaliar (80 arquivos) |
| `modelo/` | Resultado do treino: modelo, gráficos, relatórios e os `.h` do ESP32 |
| `requirements.txt` | Bibliotecas Python |
| `GUIA-IA.md` | Como tudo funciona e como adicionar sons |

## O que configurar

- Para só **retreinar**: nada além de instalar o Python 3.12 e as bibliotecas.
- Para **mudar os parâmetros do treino**: edite `treino/config.py` (tabela explicada no guia, seção 4).
- Para **adicionar um som**: siga o [checklist do guia](GUIA-IA.md#6-adicionar-um-novo-som-checklist).

## Resumo dos comandos (PowerShell, dentro desta pasta)

```powershell
# uma vez: ambiente Python 3.12 (ou use a opção 4 do setup.bat)
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r requirements.txt

# gravar sons novos (ESP32 no USB, Serial Monitor fechado)
.\.venv\Scripts\python.exe treino\coletar_serial.py COM3 palma 30

# treinar -> exportar (já copia os .h para o firmware) -> conferir
.\.venv\Scripts\python.exe treino\treinar.py
.\.venv\Scripts\python.exe treino\exportar.py
.\.venv\Scripts\python.exe treino\verificar_firmware.py
```

Depois, grave o `.ino` de novo no ESP32 (pasta `3-aparelho`).

## Resultado atual

Teste com áudios nunca vistos: **92,5%** de acerto (modelo float) e **90,0%** (int8, o que roda no
ESP32). Modelo final com 14,3 KB.
