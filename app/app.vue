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

// Dispara o alerta visual (flash + vibracao) para um evento novo
function alertar(ev: Evento) {
  flash.value = ev
  navigator.vibrate?.([200, 100, 200, 100, 200])
  setTimeout(() => {
    if (flash.value?.id === ev.id) flash.value = null
  }, 2500)
}

// Marca um evento como visto (usado por quem esta gerenciando os alertas)
async function marcarVisto(ev: Evento) {
  ev.visto = true
  await sb.from('eventos').update({ visto: true }).eq('id', ev.id)
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
    <!-- Alerta visual em tela cheia: icone + texto grande, nao so cor -->
    <div v-if="flash" class="flash" :class="flash.tipo">
      <span class="flash-icone">{{ ICONES[flash.tipo] ?? '🔊' }}</span>
      <span class="flash-texto">{{ flash.tipo.toUpperCase() }}</span>
      <span class="flash-no">{{ flash.node_id }}</span>
    </div>

    <header>
      <h1>Alerta de Sons ESP32</h1>
      <span class="status" :class="statusRealtime === 'SUBSCRIBED' ? 'on' : 'off'">
        {{ statusRealtime === 'SUBSCRIBED' ? 'ao vivo' : statusRealtime }}
      </span>
    </header>

    <!-- Status de cada no (ESP32) conhecido -->
    <section class="nos">
      <span v-for="id in nodesConhecidos" :key="id" class="no-chip" :class="noOnline(id) ? 'on' : 'off'">
        {{ id }} · {{ noOnline(id) ? 'online' : 'offline' }}
      </span>
    </section>

    <!-- Filtros do historico -->
    <section class="filtros">
      <select v-model="filtroTipo">
        <option value="todos">Todos os tipos</option>
        <option value="campainha">Campainha</option>
        <option value="palma">Palma</option>
      </select>
      <select v-model="filtroNode">
        <option value="todos">Todos os nos</option>
        <option v-for="id in nodesConhecidos" :key="id" :value="id">{{ id }}</option>
      </select>
    </section>

    <p v-if="erroMsg" class="erro">{{ erroMsg }}</p>
    <p v-else-if="!eventosFiltrados.length" class="vazio">Aguardando sons…</p>

    <ul class="lista">
      <li v-for="ev in eventosFiltrados" :key="ev.id" :class="{ visto: ev.visto }">
        <span class="icone">{{ ICONES[ev.tipo] ?? '🔊' }}</span>
        <span class="tag">{{ ev.node_id }}</span>
        <span class="tipo">{{ ev.tipo }}</span>
        <span class="hora">{{ hora(ev.created_at) }}</span>
        <button v-if="!ev.visto" class="btn-visto" @click="marcarVisto(ev)">Marcar como visto</button>
      </li>
    </ul>
  </div>
</template>

<style>
* { box-sizing: border-box; }
body { margin: 0; }
.app { min-height: 100vh; background: #0d1b2a; color: #e8eef5;
       font-family: system-ui, sans-serif; padding: 20px; }
header { display: flex; justify-content: space-between; align-items: center; gap: 12px; }
h1 { font-size: 20px; margin: 0; }
.status { font-size: 12px; padding: 2px 8px; border-radius: 6px; }
.status.on { background: #1f7a3f; }
.status.off { background: #7a3f1f; }

.nos { display: flex; gap: 8px; flex-wrap: wrap; margin-top: 16px; }
.no-chip { font-size: 12px; padding: 4px 10px; border-radius: 6px; }
.no-chip.on { background: #1f7a3f; }
.no-chip.off { background: #7a3f1f; }

.filtros { display: flex; gap: 10px; margin-top: 16px; }
.filtros select { background: #16233a; color: #e8eef5; border: 1px solid #2a3a56;
                   border-radius: 8px; padding: 8px 10px; }

.erro { background: #5a1f1f; padding: 12px 14px; border-radius: 8px; margin-top: 20px; }
.vazio { opacity: .6; margin-top: 40px; text-align: center; }

.lista { list-style: none; padding: 0; margin: 20px 0 0; }
.lista li { display: flex; gap: 12px; align-items: center; padding: 12px 14px;
            margin-bottom: 8px; background: #16233a; border-radius: 10px; }
.lista li.visto { opacity: .5; }
.icone { font-size: 20px; }
.tag { background: #3a86ff; padding: 2px 8px; border-radius: 6px; font-size: 12px; }
.tipo { font-weight: 600; text-transform: capitalize; }
.hora { margin-left: auto; opacity: .6; font-size: 13px; }
.btn-visto { background: #3a86ff; color: #fff; border: 0; border-radius: 8px;
             padding: 6px 10px; font-size: 12px; cursor: pointer; }

/* Alerta em tela cheia: fonte enorme, alto contraste, icone + texto */
.flash { position: fixed; inset: 0; z-index: 999; display: flex; flex-direction: column;
         align-items: center; justify-content: center; gap: 10px;
         animation: pisca .4s steps(2, start) infinite; }
.flash.campainha { background: #ffb300; color: #1a1300; }
.flash.palma { background: #ff3b3b; color: #fff; }
.flash-icone { font-size: 120px; }
.flash-texto { font-size: 48px; font-weight: 800; letter-spacing: 2px; }
.flash-no { font-size: 20px; opacity: .8; }
@keyframes pisca { 0% { opacity: 1; } 100% { opacity: .55; } }
</style>
