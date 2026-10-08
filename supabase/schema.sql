-- Rode este script no SQL Editor do Supabase (painel do projeto -> SQL Editor -> New query).
-- Ele cria as duas tabelas usadas pelo projeto e libera o acesso minimo necessario.

-- Tabela de eventos de som detectados pelos ESP32
create table if not exists public.eventos (
  id bigint generated always as identity primary key,
  node_id text not null,
  tipo text not null,
  intensidade int,
  visto boolean not null default false,
  created_at timestamptz not null default now()
);

-- Tabela com o ultimo sinal de vida de cada ESP32 (para status online/offline)
create table if not exists public.nodes (
  node_id text primary key,
  last_seen timestamptz not null default now()
);

-- RLS: o app usa a chave anon, entao precisa de permissao explicita
alter table public.eventos enable row level security;
alter table public.nodes enable row level security;

-- Leitura publica das duas tabelas (o app le sem estar logado)
create policy "eventos: leitura publica" on public.eventos for select using (true);
create policy "nodes: leitura publica" on public.nodes for select using (true);

-- Insercao publica (o ESP32 grava eventos e o heartbeat sem estar logado)
create policy "eventos: insercao publica" on public.eventos for insert with check (true);
create policy "nodes: insercao publica" on public.nodes for insert with check (true);

-- Atualizacao publica (marcar evento como visto / atualizar o heartbeat do no)
create policy "eventos: atualizacao publica" on public.eventos for update using (true);
create policy "nodes: atualizacao publica" on public.nodes for update using (true);

-- Delecao publica (limpar eventos de teste, remover um no antigo)
create policy "eventos: delecao publica" on public.eventos for delete using (true);
create policy "nodes: delecao publica" on public.nodes for delete using (true);

-- Realtime: sem isso o app nao recebe os eventos ao vivo
alter publication supabase_realtime add table public.eventos;
alter publication supabase_realtime add table public.nodes;

-- Tabela de configuracao (so uma linha fixa, id = 1) com o chat_id do
-- Telegram de quem recebe os alertas no celular
create table if not exists public.config (
  id int primary key default 1,
  telegram_chat_id text,
  constraint config_singleton check (id = 1)
);
insert into public.config (id) values (1) on conflict (id) do nothing;

alter table public.config enable row level security;
create policy "config: leitura publica" on public.config for select using (true);
create policy "config: atualizacao publica" on public.config for update using (true);
