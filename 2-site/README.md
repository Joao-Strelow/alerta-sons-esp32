# 2 · Site (app web)

**Para que serve:** mostra os alertas em tempo real. É a tela que a pessoa vê: um alerta gigante que
pisca ao detectar um som, o histórico e o status de cada aparelho.

Feito com Nuxt 4 (Vue 3 + TypeScript) e instalável como PWA. Todo o app está em um arquivo:
[`app/app.vue`](app/app.vue).

## O que tem aqui

| Arquivo | Função |
|---|---|
| `app/app.vue` | A tela inteira: alerta, histórico, filtros, status online/offline, campo do Telegram |
| `plugins/supabase.client.ts` | Cliente do Supabase para o navegador |
| `nuxt.config.ts` | Configuração do Nuxt e do PWA; lê a URL/chave do `.env` |
| `.env.example` | Modelo da configuração (o `.env` real é criado pelo `setup.bat`) |
| `public/` | Ícone do site |
| `package.json` | Dependências |

## O que configurar

Só o arquivo `.env` desta pasta, com a URL e a chave do Supabase:

```
NUXT_PUBLIC_SUPABASE_URL=https://seu-projeto.supabase.co
NUXT_PUBLIC_SUPABASE_KEY=chave-anon-publishable
```

O `setup.bat` (na raiz) cria esse arquivo e roda o `npm install`.

## Como rodar

```bash
npm install      # uma vez só
npm run dev      # abre em http://localhost:3000
```

Para gerar a versão final: `npm run build` (precisa do Node.js 22 ou mais novo).

## Como funciona

- **Alerta em tela:** quando chega um evento novo, a tela inteira pisca com ícone e texto grande por 3 s
  (ou até confirmar). Não depende só de cor, para acessibilidade.
- **Histórico:** últimos 50 eventos, com filtro por tipo e por aparelho e botão "marcar como visto".
- **Status:** um aparelho é *online* se mandou evento ou heartbeat nos últimos 90 s.
- **Telegram:** guarda o chat ID da pessoa na tabela `config`.

## Adicionando um som novo

No `app/app.vue`: acrescente o ícone em `ICONES`, uma `<option>` no filtro de tipo e a cor de
`.flash.<nome>` no CSS. Passo a passo em [4-ia/GUIA-IA.md](../4-ia/GUIA-IA.md#6-adicionar-um-novo-som-checklist).
