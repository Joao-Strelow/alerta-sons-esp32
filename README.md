# Alerta de Sons ESP32

Sistema de acessibilidade: um ESP32 com microfone detecta sons (campainha,
palma) usando um modelo do Edge Impulse e manda o evento para o Supabase.
Um app web (PWA em Nuxt 4) mostra, em tempo real, um alerta visual gigante
em tela (icone + texto grande + flash), junto com historico e status de
cada ESP32 (online/offline).

## Estrutura do projeto

- `firmware/alerta-sons-esp32.ino`: codigo do ESP32 (Arduino).
- `app/app.vue`: tela principal do app web.
- `plugins/supabase.client.ts`: cliente do Supabase usado pelo app.
- `supabase/schema.sql`: script para configurar o banco no Supabase.

## 1. Configurar o Supabase (passo a passo)

Você já criou o projeto no Supabase, agora falta só isso:

1. Entre no painel do seu projeto em [supabase.com](https://supabase.com).
2. No menu lateral, abra **SQL Editor** e clique em **New query**.
3. Cole todo o conteúdo do arquivo `supabase/schema.sql` deste repositório e
   clique em **Run**. Isso cria as tabelas `eventos` e `nodes`, libera o
   acesso necessário e ativa o tempo real.
4. Vá em **Project Settings -> Data API** (ou **API** em versões mais
   antigas) e copie:
   - o **Project URL**;
   - a chave **anon public** (nunca a `service_role`).

Esses dois valores são usados tanto no app web quanto no firmware do ESP32.

## 2. Configurar o app web

Crie um arquivo `.env` na raiz do projeto (copie de `.env.example`):

```bash
NUXT_PUBLIC_SUPABASE_URL=https://<seu-projeto>.supabase.co
NUXT_PUBLIC_SUPABASE_KEY=<chave anon/publishable>
```

Instale as dependências e suba o app:

```bash
pnpm install
pnpm dev
```

O app abre em `http://localhost:3000`.

> Requer Node.js 22 ou superior para `pnpm build` funcionar sem erros
> (algumas dependências usam recursos novos do JavaScript). `pnpm dev`
> funciona normalmente em versões mais antigas.

## 3. Configurar o firmware do ESP32

Abra `firmware/alerta-sons-esp32.ino` na Arduino IDE e preencha, no topo do
arquivo:

- `WIFI_SSID` / `WIFI_PASS`: sua rede 2.4 GHz;
- `SUPABASE_URL` / `SUPABASE_KEY`: os mesmos valores do passo 1;
- `NODE_ID`: um nome único para esse ESP32 (ex.: `"no1"`, `"porta"`).

Grave no ESP32. No monitor serial, envie `s 1` para ligar o envio dos
eventos ao Supabase (por padrão ele começa desligado).

O firmware manda, a cada 30 segundos, um "sinal de vida" para a tabela
`nodes` (independente de detectar som ou não), o que permite ao app saber
se aquele ESP32 está online.

## Como funciona o gerenciamento no app

- **Alerta em tela**: ao chegar um evento, a tela inteira pisca com um
  ícone e um texto enorme (campainha ou palma), sem depender só de cor.
- **Histórico**: lista os últimos eventos, com filtro por tipo e por nó.
- **Marcar como visto**: cada evento pode ser marcado como visto, para
  diferenciar do que ainda não foi notado.
- **Status por nó**: mostra se cada ESP32 está online ou offline, com base
  no heartbeat.
