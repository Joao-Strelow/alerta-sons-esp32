<script setup lang="ts">
import { createClient, type SupabaseClient } from '@supabase/supabase-js'

type Evento = {
  id: number
  node_id: string
  tipo: string
  intensidade: number | null
  visto: boolean
  created_at: string
}

type Node = {
  node_id: string
  last_seen: string
}

// Depois de quanto tempo sem sinal um no e considerado offline (ms)
const LIMITE_OFFLINE_MS = 90_000

// Como mostrar cada tipo de som: icone + texto grande, nunca so cor
// (acessibilidade: nao depender de percepcao de cor)
const ICONES: Record<string, string> = {
  campainha: '🔔',
  palma: '👏',
}

const eventos = ref<Evento[]>([])
const nodes = ref<Node[]>([])
const flash = ref<Evento | null>(null)
const statusRealtime = ref('desconectado')
const erroMsg = ref('')
const filtroTipo = ref('todos')
const filtroNode = ref('todos')
const agora = ref(Date.now())

let sb: SupabaseClient

// Lista de nos distintos que ja apareceram, para montar o filtro
const nodesConhecidos = computed(() => {
  const ids = new Set(eventos.value.map((e) => e.node_id))
  nodes.value.forEach((n) => ids.add(n.node_id))
  return [...ids]
})

// Aplica os filtros de tipo e no sobre a lista de eventos
const eventosFiltrados = computed(() =>
  eventos.value.filter(
    (e) =>
      (filtroTipo.value === 'todos' || e.tipo === filtroTipo.value) &&
      (filtroNode.value === 'todos' || e.node_id === filtroNode.value),
  ),
)

// Um no e "online" se mandou algo (evento ou heartbeat) ha pouco tempo
function noOnline(nodeId: string) {
  const n = nodes.value.find((x) => x.node_id === nodeId)
  if (!n) return false
  return agora.value - new Date(n.last_seen).getTime() < LIMITE_OFFLINE_MS
}

const hora = (iso: string) => new Date(iso).toLocaleTimeString('pt-BR')

// Dispara o alerta visual (fica piscando ate confirmar ou chegar outro evento)
function alertar(ev: Evento) {
  flash.value = ev
  navigator.vibrate?.([200, 100, 200, 100, 200])
}

// Marca um evento como visto (usado tanto pela lista quanto pelo botao do alerta)
async function marcarVisto(ev: Evento) {
  ev.visto = true
  await sb.from('eventos').update({ visto: true }).eq('id', ev.id)
}

// Confirma que a pessoa viu o alerta em tela cheia e fecha ele
function confirmarAlerta() {
  if (flash.value) marcarVisto(flash.value)
  flash.value = null
}

// Busca o historico de eventos e o estado atual dos nos
async function carregarDados() {
  const [{ data: ev, error: erroEv }, { data: nd, error: erroNd }] = await Promise.all([
    sb.from('eventos').select('*').order('created_at', { ascending: false }).limit(50),
    sb.from('nodes').select('*'),
  ])
  if (erroEv || erroNd) {
    erroMsg.value = `Erro ao carregar dados: ${erroEv?.message ?? erroNd?.message}`
    return
  }
  eventos.value = (ev ?? []) as Evento[]
  nodes.value = (nd ?? []) as Node[]
}

// Assina as mudancas em tempo real das tabelas eventos e nodes
function assinarRealtime() {
  sb.channel('eventos-realtime')
    .on('postgres_changes', { event: 'INSERT', schema: 'public', table: 'eventos' }, (payload) => {
      const ev = payload.new as Evento
      eventos.value.unshift(ev)
      if (eventos.value.length > 50) eventos.value.pop()   // mantem a lista curta e leve
      alertar(ev)
    })
    .on('postgres_changes', { event: '*', schema: 'public', table: 'nodes' }, (payload) => {
      const no = payload.new as Node
      const i = nodes.value.findIndex((n) => n.node_id === no.node_id)
      if (i >= 0) nodes.value[i] = no
      else nodes.value.push(no)
    })
    .subscribe((status) => (statusRealtime.value = status))
}

onMounted(async () => {
  const cfg = useRuntimeConfig()
  const url = cfg.public.supabaseUrl as string
  const key = cfg.public.supabaseKey as string

  if (!url || !key) {
    erroMsg.value = 'URL ou key do Supabase vazias — confira o .env e reinicie o pnpm dev.'
    return
  }

  sb = createClient(url, key)
  await carregarDados()
  assinarRealtime()

  // atualiza o relogio interno para recalcular quem esta online/offline
  setInterval(() => (agora.value = Date.now()), 5000)
})
</script>

<template>
  <div class="app">
    <!-- Alerta visual em tela cheia: pisca ate confirmar ou chegar outro evento -->
    <div v-if="flash" class="flash" :class="flash.tipo" @click="confirmarAlerta">
      <span class="flash-icone">{{ ICONES[flash.tipo] ?? '🔊' }}</span>
      <span class="flash-texto">{{ flash.tipo.toUpperCase() }}</span>
      <span class="flash-no">{{ flash.node_id }}</span>
      <button class="btn-confirmar" @click.stop="confirmarAlerta">✓ Confirmar que vi</button>
    </div>

    <header>
      <div class="titulo">
        <h1>🔊 Alerta de Sons</h1>
        <p class="subtitulo">Monitoramento acessível por ESP32 + Supabase</p>
      </div>
      <span class="status" :class="statusRealtime === 'SUBSCRIBED' ? 'on' : 'off'">
        <span class="bolinha" />
        {{ statusRealtime === 'SUBSCRIBED' ? 'ao vivo' : statusRealtime }}
      </span>
    </header>

    <!-- Status de cada no (ESP32) conhecido -->
    <section class="cartao">
      <h2>Dispositivos</h2>
      <div class="nos">
        <span v-for="id in nodesConhecidos" :key="id" class="no-chip" :class="noOnline(id) ? 'on' : 'off'">
          <span class="bolinha" />
          {{ id }} · {{ noOnline(id) ? 'online' : 'offline' }}
        </span>
        <span v-if="!nodesConhecidos.length" class="vazio-inline">Nenhum dispositivo visto ainda</span>
      </div>
    </section>

    <!-- Historico de eventos com filtros -->
    <section class="cartao">
      <div class="cartao-header">
        <h2>Histórico</h2>
        <div class="filtros">
          <select v-model="filtroTipo">
            <option value="todos">Todos os tipos</option>
            <option value="campainha">Campainha</option>
            <option value="palma">Palma</option>
          </select>
          <select v-model="filtroNode">
            <option value="todos">Todos os nós</option>
            <option v-for="id in nodesConhecidos" :key="id" :value="id">{{ id }}</option>
          </select>
        </div>
      </div>

      <p v-if="erroMsg" class="erro">{{ erroMsg }}</p>
      <p v-else-if="!eventosFiltrados.length" class="vazio">Aguardando sons…</p>

      <ul v-else class="lista">
        <li v-for="ev in eventosFiltrados" :key="ev.id" :class="{ visto: ev.visto }">
          <span class="icone">{{ ICONES[ev.tipo] ?? '🔊' }}</span>
          <div class="info">
            <span class="tipo">{{ ev.tipo }}</span>
            <span class="tag">{{ ev.node_id }}</span>
          </div>
          <span class="hora">{{ hora(ev.created_at) }}</span>
          <button v-if="!ev.visto" class="btn-visto" @click="marcarVisto(ev)">Marcar como visto</button>
          <span v-else class="ja-visto">visto</span>
        </li>
      </ul>
    </section>
  </div>
</template>

<style>
* { box-sizing: border-box; }
body { margin: 0; }

.app { min-height: 100vh; background: #0b1622; color: #e8eef5;
       font-family: system-ui, -apple-system, sans-serif;
       padding: 24px 16px 60px; }

header { max-width: 720px; margin: 0 auto 24px; display: flex;
         justify-content: space-between; align-items: flex-start; gap: 12px; }
h1 { font-size: 26px; margin: 0; letter-spacing: -.5px; }
.subtitulo { margin: 4px 0 0; font-size: 13px; opacity: .6; }

.status { display: flex; align-items: center; gap: 6px; font-size: 12px;
          font-weight: 600; padding: 6px 12px; border-radius: 20px; white-space: nowrap; }
.status.on { background: #113a24; color: #4ade80; }
.status.off { background: #3a1f1f; color: #f87171; }
.bolinha { width: 7px; height: 7px; border-radius: 50%; background: currentColor; }

.cartao { max-width: 720px; margin: 0 auto 16px; background: #101d2e;
          border: 1px solid #1c2c42; border-radius: 16px; padding: 18px 20px; }
.cartao h2 { font-size: 14px; margin: 0 0 12px; opacity: .7; text-transform: uppercase; letter-spacing: .5px; }
.cartao-header { display: flex; flex-wrap: wrap; justify-content: space-between;
                  align-items: center; gap: 10px; margin-bottom: 4px; }
.cartao-header h2 { margin: 0; }

.nos { display: flex; gap: 8px; flex-wrap: wrap; }
.no-chip { display: flex; align-items: center; gap: 6px; font-size: 13px;
           font-weight: 500; padding: 6px 12px; border-radius: 20px; }
.no-chip.on { background: #113a24; color: #4ade80; }
.no-chip.off { background: #3a1f1f; color: #f87171; }
.vazio-inline { font-size: 13px; opacity: .5; }

.filtros { display: flex; gap: 8px; }
.filtros select { background: #16233a; color: #e8eef5; border: 1px solid #2a3a56;
                   border-radius: 8px; padding: 8px 10px; font-size: 13px; }

.erro { background: #3a1f1f; color: #fca5a5; padding: 12px 14px; border-radius: 10px; }
.vazio { opacity: .5; padding: 24px 0; text-align: center; font-size: 14px; }

.lista { list-style: none; padding: 0; margin: 4px 0 0; display: flex; flex-direction: column; gap: 8px; }
.lista li { display: flex; gap: 12px; align-items: center; padding: 12px 14px;
            background: #16233a; border-radius: 12px; transition: opacity .2s;
            contain: content; }
.lista li.visto { opacity: .45; }
.icone { font-size: 24px; line-height: 1; }
.info { display: flex; flex-direction: column; gap: 2px; }
.tipo { font-weight: 600; text-transform: capitalize; font-size: 14px; }
.tag { font-size: 11px; opacity: .55; }
.hora { margin-left: auto; opacity: .55; font-size: 12px; }
.btn-visto { background: #3a86ff; color: #fff; border: 0; border-radius: 8px;
             padding: 7px 12px; font-size: 12px; font-weight: 600; cursor: pointer; }
.btn-visto:hover { background: #5b9dff; }
.ja-visto { font-size: 11px; opacity: .5; }

/* Alerta em tela cheia: fonte enorme, alto contraste, icone + texto */
.flash { position: fixed; inset: 0; z-index: 999; display: flex; flex-direction: column;
         align-items: center; justify-content: center; gap: 10px;
         animation: pisca .4s steps(2, start) infinite;
         will-change: opacity; }
.flash.campainha { background: #ffb300; color: #1a1300; }
.flash.palma { background: #ff3b3b; color: #fff; }
.flash-icone { font-size: 130px; filter: drop-shadow(0 8px 24px rgba(0,0,0,.25)); }
.flash-texto { font-size: 48px; font-weight: 800; letter-spacing: 2px; }
.flash-no { font-size: 18px; opacity: .8; }
.btn-confirmar { margin-top: 24px; background: rgba(0, 0, 0, .25); color: inherit;
                 border: 2px solid currentColor; border-radius: 999px;
                 padding: 14px 28px; font-size: 18px; font-weight: 700; cursor: pointer; }
.btn-confirmar:hover { background: rgba(0, 0, 0, .4); }
@keyframes pisca { 0% { opacity: 1; } 100% { opacity: .55; } }

@media (max-width: 480px) {
  h1 { font-size: 22px; }
  .flash-icone { font-size: 90px; }
  .flash-texto { font-size: 34px; }
  .btn-confirmar { font-size: 16px; padding: 12px 22px; }
}
</style>
