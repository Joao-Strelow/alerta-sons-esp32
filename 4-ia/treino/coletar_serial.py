"""
coletar_serial.py  -  GRAVA NOVOS SONS PELO ESP32 E SALVA NO PC
================================================================

Para melhorar a IA precisamos de mais exemplos de áudio. Este script pede ao
ESP32 que grave sons e recebe os áudios pelo cabo USB, salvando arquivos .wav
na pasta dados/ (sem usar nenhuma plataforma na internet).

Como usar (feche o Serial Monitor do Arduino antes: só um programa pode usar
a porta COM por vez):

    .venv\\Scripts\\python.exe treino\\coletar_serial.py COM3 palma 30
    .venv\\Scripts\\python.exe treino\\coletar_serial.py COM3 campainha 30
    .venv\\Scripts\\python.exe treino\\coletar_serial.py COM3 ruido 60

  COM3      = porta do ESP32 (veja no Arduino IDE, em Ferramentas > Porta)
  palma     = classe a gravar: palma, campainha ou ruido
  30        = quantidade de gravações de 1 segundo

Cores do LED do ESP32 durante cada gravação:
  roxo = prepare-se | branco = GRAVANDO, faça o som agora | azul = enviando | verde = ok

Divisão treino/teste: a cada 5 gravações, 1 vai para a pasta de TESTE (20%) e as
demais para a de TREINO - a mesma proporção usada antes.
"""

import sys
import time
import wave

import serial   # biblioteca pyserial: comunica com a porta USB/serial

import config

TAXA_SERIAL = 115200              # igual ao Serial.begin() do firmware
MARCA_INICIO = b"<<WAV "          # o firmware envia: <<WAV classe tamanho>>
MARCA_FIM = b"<<FIM>>"


def ler_ate(porta, marca, limite_s):
    """Lê bytes da porta até encontrar 'marca'. Devolve tudo o que veio antes dela."""
    buffer = bytearray()
    prazo = time.time() + limite_s
    while time.time() < prazo:
        dados = porta.read(porta.in_waiting or 1)
        if dados:
            buffer += dados
            posicao = buffer.find(marca)
            if posicao >= 0:
                return bytes(buffer[:posicao]), bytes(buffer[posicao:])
    raise TimeoutError(f"o ESP32 não respondeu a tempo (esperando {marca!r})")


def salvar_wav(caminho, audio_bytes):
    """Salva os bytes recebidos (int16, mono, 16 kHz) como arquivo .wav."""
    with wave.open(str(caminho), "wb") as arq:
        arq.setnchannels(1)                        # mono
        arq.setsampwidth(2)                        # 2 bytes = 16 bits por amostra
        arq.setframerate(config.TAXA_AMOSTRAGEM)   # 16.000 amostras por segundo
        arq.writeframes(audio_bytes)


def main():
    if len(sys.argv) != 4 or sys.argv[2] not in config.CLASSES:
        print(__doc__)
        sys.exit(1)
    nome_porta, classe, quantidade = sys.argv[1], sys.argv[2], int(sys.argv[3])

    # Letra do comando no firmware: c = campainha, p = palma, r = ruido
    letra = {"campainha": "c", "palma": "p", "ruido": "r"}[classe]

    # Contagem inicial para manter a regra "1 a cada 5 vai para teste"
    contador = len(list(config.PASTA_TREINO.glob(f"{classe}.*.wav"))) + \
               len(list(config.PASTA_TESTE.glob(f"{classe}.*.wav")))

    with serial.Serial(nome_porta, TAXA_SERIAL, timeout=0.1) as porta:
        time.sleep(0.5)
        porta.reset_input_buffer()
        print(f"Pedindo {quantidade} gravações de '{classe}'. Observe o LED do ESP32.")
        porta.write(f"{letra} {quantidade}\n".encode())

        recebidas = 0
        resto = b""
        while recebidas < quantidade:
            # 1) espera o início de um áudio (marca "<<WAV classe tamanho>>")
            _, resto2 = ler_ate(porta, MARCA_INICIO, limite_s=30) if not resto.startswith(MARCA_INICIO) \
                else (b"", resto)
            while b">>\n" not in resto2:                        # garante a linha de cabeçalho inteira
                resto2 += porta.read(porta.in_waiting or 1)
            cabecalho, depois = resto2.split(b">>\n", 1)
            tamanho = int(cabecalho.decode().split()[-1])        # quantos bytes de áudio vêm

            # 2) lê exatamente 'tamanho' bytes de áudio
            audio = bytearray(depois)
            prazo = time.time() + 30
            while len(audio) < tamanho + len(b"\n<<FIM>>\n") and time.time() < prazo:
                audio += porta.read(porta.in_waiting or 1)
            if len(audio) < tamanho:
                raise TimeoutError("áudio incompleto")
            dados_audio, resto = bytes(audio[:tamanho]), bytes(audio[tamanho:])

            # 3) salva: 1 a cada 5 vai para a pasta de teste
            pasta = config.PASTA_TESTE if contador % 5 == 4 else config.PASTA_TREINO
            nome = f"{classe}.{int(time.time() * 1000)}.wav"
            salvar_wav(pasta / nome, dados_audio)
            contador += 1
            recebidas += 1
            print(f"[{recebidas}/{quantidade}] salvo em {pasta.name}/{nome}")

    print("Concluído. Rode treino/treinar.py para treinar com os novos dados.")


if __name__ == "__main__":
    main()
