# 1 · Banco de dados (Supabase)

**Para que serve:** guarda os eventos detectados pelo ESP32 e avisa o site em tempo real.
É o "correio" entre o aparelho e o aviso: o ESP32 grava, o site e o Telegram recebem.

## O que tem aqui

| Arquivo | Função |
|---|---|
| `schema.sql` | Cria as tabelas, as regras de acesso (RLS) e liga o tempo real |
| `telegram/` | **Opcional.** Edge Function que manda o alerta ao Telegram |

## O que configurar

1. Criar uma conta e um projeto em [supabase.com](https://supabase.com) (grátis).
2. Rodar o `schema.sql` no **SQL Editor** (o `setup.bat` copia o SQL e abre a página para você).
3. Anotar o **Project URL** e a chave **anon / publishable** em *Project Settings → Data API*.
   Nunca use a `service_role`.

## Tabelas criadas

| Tabela | Conteúdo |
|---|---|
| `eventos` | Cada som detectado: `node_id`, `tipo`, `intensidade`, `visto`, `created_at` |
| `nodes` | Último "estou vivo" de cada aparelho (`last_seen`), para mostrar online/offline |
| `config` | Uma linha só, com o `telegram_chat_id` de quem recebe os alertas |

A coluna `tipo` aceita qualquer texto, então **adicionar um som novo não exige mudar o banco**.

## Como saber se está certo

Rode `node setup.mjs --testar` na raiz do projeto. Ele confirma que o Supabase responde e que as 3
tabelas existem.

> **Segurança:** as regras criadas liberam leitura, escrita e deleção a quem tiver a chave pública.
> É suficiente para um projeto de estudo; para uso real, restrinja com login.
