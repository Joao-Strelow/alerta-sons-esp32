#!/usr/bin/env node
// ============================================================================
// APP DE SETUP - Alerta de Sons
// ============================================================================
// Assistente no terminal que configura o projeto para você:
//   - confere o que está instalado (Node, Python, Git, Arduino IDE);
//   - pede a URL e a chave do Supabase e TESTA se funcionam;
//   - cria o site (.env) e o firmware (secrets.h) com os mesmos valores;
//   - copia o SQL do banco para a área de transferência e abre o painel;
//   - instala as dependências do site (npm) e da IA (Python, opcional);
//   - permite editar a configuração depois (trocar de banco, nome do aparelho,
//     envio ao ligar) e guardar vários bancos para alternar entre eles.
//
// Como usar: dê dois cliques em "setup.bat" (ou rode "node setup.mjs").
// Opções:   --verificar   só mostra o estado atual, sem perguntar nada
//           --testar      só testa a conexão com o Supabase
// Não usa nenhuma biblioteca externa: só o Node.js.
// ============================================================================

import fs from 'node:fs'
import path from 'node:path'
import os from 'node:os'
import readline from 'node:readline/promises'
import { spawnSync } from 'node:child_process'
import { fileURLToPath } from 'node:url'

const RAIZ = path.dirname(fileURLToPath(import.meta.url))
const PASTA_BANCO = path.join(RAIZ, '1-banco')
const PASTA_SITE = path.join(RAIZ, '2-site')
const PASTA_SKETCH = path.join(RAIZ, '3-aparelho', 'alerta-sons-ia')
const PASTA_IA = path.join(RAIZ, '4-ia')

const ARQ_SCHEMA = path.join(PASTA_BANCO, 'schema.sql')
const ARQ_ENV = path.join(PASTA_SITE, '.env')
const ARQ_SECRETS = path.join(PASTA_SKETCH, 'secrets.h')

const WIN = process.platform === 'win32'
const ARGS = process.argv.slice(2)

// ---------------------------------------------------------------------------
// Texto colorido e perguntas
// ---------------------------------------------------------------------------
const c = (cod, t) => (process.stdout.isTTY ? `\x1b[${cod}m${t}\x1b[0m` : t)
const verde = (t) => c('32', t)
const vermelho = (t) => c('31', t)
const amarelo = (t) => c('33', t)
const ciano = (t) => c('36', t)
const negrito = (t) => c('1', t)
const cinza = (t) => c('90', t)

const OK = verde('✔')
const ERRO = vermelho('✘')
const AVISO = amarelo('!')
const OPC = cinza('○')

const rl = readline.createInterface({ input: process.stdin, output: process.stdout })
let entradaEncerrada = false
rl.on('close', () => { entradaEncerrada = true })

async function perguntar(texto, padrao = '') {
  if (entradaEncerrada) return padrao
  const dica = padrao ? cinza(` [${padrao}]`) : ''
  try {
    const r = (await rl.question(`${texto}${dica}: `)).trim()
    return r || padrao
  } catch {
    return padrao
  }
}

async function confirmar(texto, padraoSim = true) {
  const r = (await perguntar(`${texto} ${cinza(padraoSim ? '(S/n)' : '(s/N)')}`)).toLowerCase()
  if (!r) return padraoSim
  return r.startsWith('s') || r === 'y'
}

function titulo(t) {
  console.log('\n' + negrito(ciano(`━━ ${t} ${'━'.repeat(Math.max(2, 60 - t.length))}`)))
}

function linhaRelatorio(simbolo, texto, detalhe = '') {
  console.log(`  ${simbolo} ${texto}${detalhe ? cinza('  ' + detalhe) : ''}`)
}

// ---------------------------------------------------------------------------
// Utilidades de sistema
// ---------------------------------------------------------------------------
// No Windows o npm e o py são atalhos (.cmd) e exigem shell; juntamos o comando
// numa string só para o Node não reclamar de argumentos sem escape.
function rodar(cmd, args, opcoes = {}) {
  if (WIN) return spawnSync([cmd, ...args].join(' '), { encoding: 'utf8', shell: true, ...opcoes })
  return spawnSync(cmd, args, { encoding: 'utf8', ...opcoes })
}

function versaoDe(cmd, args = ['--version']) {
  const r = rodar(cmd, args)
  if (r.status !== 0 || r.error) return null
  return (r.stdout || r.stderr || '').trim().split('\n')[0]
}

function acharPython() {
  const candidatos = WIN
    ? [['py', ['-3.12']], ['python', []], ['py', ['-3']]]
    : [['python3.12', []], ['python3', []], ['python', []]]
  for (const [cmd, pre] of candidatos) {
    const v = versaoDe(cmd, [...pre, '--version'])
    if (v && /Python 3\.\d+/.test(v)) return { cmd, pre, versao: v }
  }
  return null
}

// Devolve { origem } se achar a Arduino IDE, ou null. Cobre a versão baixada do site
// (2.x e a antiga 1.8), a do macOS e a da Microsoft Store (que fica em WindowsApps,
// uma pasta que não dá para listar, então é perguntada ao Windows).
function acharArduinoIDE() {
  const pf = process.env.ProgramFiles || ''
  const pf86 = process.env['ProgramFiles(x86)'] || ''
  const locais = [
    path.join(process.env.LOCALAPPDATA || '', 'Programs', 'Arduino IDE', 'Arduino IDE.exe'),
    path.join(pf, 'Arduino IDE', 'Arduino IDE.exe'),
    path.join(pf86, 'Arduino IDE', 'Arduino IDE.exe'),
    path.join(pf, 'Arduino', 'arduino.exe'),
    path.join(pf86, 'Arduino', 'arduino.exe'),
    '/Applications/Arduino IDE.app',
    '/Applications/Arduino.app',
  ]
  const achado = locais.find((p) => p && fs.existsSync(p))
  if (achado) return { origem: achado }

  if (WIN) {
    const r = rodar('powershell', ['-NoProfile', '-Command',
      '"(Get-AppxPackage -Name *Arduino* | Select-Object -First 1).Version"'])
    const versao = (r.stdout || '').trim()
    if (r.status === 0 && /^\d+(\.\d+)+$/.test(versao)) {
      return { origem: `Microsoft Store, versão ${versao.split('.').slice(0, 3).join('.')}` }
    }
  }
  return null
}

function copiarParaAreaDeTransferencia(texto) {
  if (WIN) return rodar('clip', [], { input: texto }).status === 0
  if (process.platform === 'darwin') return rodar('pbcopy', [], { input: texto }).status === 0
  return rodar('xclip', ['-selection', 'clipboard'], { input: texto }).status === 0
}

function abrirNoNavegador(url) {
  if (WIN) rodar('cmd', ['/c', 'start', '""', url])
  else if (process.platform === 'darwin') rodar('open', [url])
  else rodar('xdg-open', [url])
}

// ---------------------------------------------------------------------------
// Ler e gravar os arquivos de configuração
// ---------------------------------------------------------------------------
function lerEnv() {
  const r = { url: '', chave: '' }
  if (!fs.existsSync(ARQ_ENV)) return r
  const txt = fs.readFileSync(ARQ_ENV, 'utf8')
  r.url = (txt.match(/^NUXT_PUBLIC_SUPABASE_URL=(.*)$/m)?.[1] || '').trim()
  r.chave = (txt.match(/^NUXT_PUBLIC_SUPABASE_KEY=(.*)$/m)?.[1] || '').trim()
  return r
}

function lerSecrets() {
  const r = { url: '', chave: '', nodeId: '', enviarPadrao: false }
  if (!fs.existsSync(ARQ_SECRETS)) return r
  const txt = fs.readFileSync(ARQ_SECRETS, 'utf8')
  r.url = txt.match(/SUPABASE_URL\s*=\s*"([^"]*)"/)?.[1] || ''
  r.chave = txt.match(/SUPABASE_KEY\s*=\s*"([^"]*)"/)?.[1] || ''
  r.nodeId = txt.match(/#define\s+NODE_ID\s+"([^"]*)"/)?.[1] || ''
  r.enviarPadrao = /#define\s+ENVIAR_SUPABASE_PADRAO\s+true/.test(txt)
  return r
}

function gravarEnv(url, chave) {
  const txt =
    '# Gerado pelo setup.bat. Valores do painel do Supabase (Project Settings -> Data API).\n' +
    `NUXT_PUBLIC_SUPABASE_URL=${url}\n` +
    `NUXT_PUBLIC_SUPABASE_KEY=${chave}\n`
  fs.writeFileSync(ARQ_ENV, txt)
}

function gravarSecrets({ url, chave, nodeId, enviarPadrao }) {
  const txt = `// ============================================================================
// secrets.h - gerado pelo setup.bat. Não vai para o GitHub (está no .gitignore).
// Para mudar algo, rode o setup de novo ou edite aqui e grave o firmware outra vez.
// ============================================================================
#pragma once

// Nome deste aparelho. Use um nome diferente em cada ESP32.
#define NODE_ID "${nodeId}"

// Painel do Supabase -> Project Settings -> Data API
const char* SUPABASE_URL = "${url}";
const char* SUPABASE_KEY = "${chave}";

// true = já começa enviando ao Supabase; false = começa desligado (liga com "s 1").
#define ENVIAR_SUPABASE_PADRAO ${enviarPadrao ? 'true' : 'false'}
`
  fs.writeFileSync(ARQ_SECRETS, txt)
}

// ---------------------------------------------------------------------------
// Validação das credenciais do Supabase
// ---------------------------------------------------------------------------
function normalizarUrl(u) {
  return u.trim().replace(/\/+$/, '').replace(/\/rest\/v1$/, '')
}

function validarUrl(u) {
  if (!/^https:\/\/[a-z0-9-]+\.supabase\.co$/i.test(u)) {
    return 'A URL deve ser parecida com https://abcdefgh.supabase.co (Project URL do painel).'
  }
  return null
}

function validarChave(k) {
  if (k.length < 20) return 'A chave parece curta demais. Copie a "anon / publishable" inteira.'
  if (/^sb_secret_/.test(k)) return 'Essa é uma chave SECRETA. Use a "publishable" (nunca a secret/service_role).'
  if (k.startsWith('eyJ')) {
    try {
      const corpo = JSON.parse(Buffer.from(k.split('.')[1], 'base64url').toString('utf8'))
      if (corpo.role === 'service_role') return 'Essa é a chave service_role. Use a anon/publishable.'
    } catch { /* se não der para ler, deixa passar */ }
  }
  return null
}

// Devolve { ok, mensagem } depois de tentar ler uma tabela pela API REST.
async function testarTabela(url, chave, tabela) {
  try {
    const resp = await fetch(`${url}/rest/v1/${tabela}?select=*&limit=1`, {
      headers: { apikey: chave, Authorization: `Bearer ${chave}` },
      signal: AbortSignal.timeout(10000),
    })
    if (resp.ok) return { ok: true }
    if (resp.status === 401 || resp.status === 403) return { ok: false, motivo: 'chave', mensagem: 'chave recusada (401/403)' }
    if (resp.status === 404) return { ok: false, motivo: 'tabela', mensagem: `tabela "${tabela}" não existe (404)` }
    return { ok: false, motivo: 'outro', mensagem: `resposta HTTP ${resp.status}` }
  } catch (e) {
    return { ok: false, motivo: 'rede', mensagem: `sem conexão com o Supabase (${e.cause?.code || e.message})` }
  }
}

async function testarSupabase(url, chave, { mostrar = true } = {}) {
  const resultados = {}
  for (const tabela of ['eventos', 'nodes', 'config']) {
    resultados[tabela] = await testarTabela(url, chave, tabela)
  }
  const todas = Object.values(resultados)
  const rede = todas.find((r) => r.motivo === 'rede')
  const chaveRuim = todas.find((r) => r.motivo === 'chave')
  const faltando = Object.entries(resultados).filter(([, r]) => r.motivo === 'tabela').map(([t]) => t)
  const estado = rede ? 'rede' : chaveRuim ? 'chave' : faltando.length ? 'tabelas' : todas.every((r) => r.ok) ? 'ok' : 'outro'

  if (mostrar) {
    if (estado === 'ok') linhaRelatorio(OK, 'Supabase respondeu e as 3 tabelas existem (eventos, nodes, config).')
    else if (estado === 'rede') linhaRelatorio(ERRO, rede.mensagem, 'confira a internet e a URL')
    else if (estado === 'chave') linhaRelatorio(ERRO, 'A chave foi recusada pelo Supabase.', 'confira se copiou a anon/publishable inteira')
    else if (estado === 'tabelas') linhaRelatorio(AVISO, `Conectou, mas faltam tabelas: ${faltando.join(', ')}.`, 'rode o 1-banco/schema.sql no SQL Editor')
    else linhaRelatorio(AVISO, 'Resposta inesperada do Supabase.', JSON.stringify(resultados))
  }
  return { estado, resultados, faltando }
}

// ---------------------------------------------------------------------------
// Estado atual (painel)
// ---------------------------------------------------------------------------
function mostrarRequisitos() {
  titulo('O que está instalado neste computador')

  const node = process.versions.node
  const major = Number(node.split('.')[0])
  linhaRelatorio(major >= 22 ? OK : ERRO, `Node.js ${node}`, major >= 22 ? '' : 'precisa ser a versão 22 ou mais nova (nodejs.org)')

  const npm = versaoDe('npm')
  linhaRelatorio(npm ? OK : ERRO, npm ? `npm ${npm}` : 'npm não encontrado', npm ? '' : 'vem junto com o Node.js')

  const git = versaoDe('git')
  linhaRelatorio(git ? OK : AVISO, git || 'Git não encontrado', git ? '' : 'só é preciso para baixar/atualizar o projeto')

  const ide = acharArduinoIDE()
  linhaRelatorio(ide ? OK : AVISO, ide ? 'Arduino IDE encontrada' : 'Arduino IDE não encontrada',
    ide ? (/[\\/]/.test(ide.origem) ? '' : ide.origem) : 'necessária para gravar o ESP32 (arduino.cc/en/software)')

  const py = acharPython()
  if (py) {
    const ruim = !/Python 3\.(1[0-3])/.test(py.versao)
    linhaRelatorio(ruim ? AVISO : OK, py.versao, ruim ? 'para treinar a IA, use o Python 3.12' : 'opcional: só para treinar a IA')
  } else {
    linhaRelatorio(OPC, 'Python não encontrado', 'opcional: só é preciso se for treinar a IA (python.org, versão 3.12)')
  }
  return { py }
}

function mostrarEstadoProjeto() {
  titulo('Estado da configuração do projeto')
  const env = lerEnv()
  const sec = lerSecrets()

  linhaRelatorio(env.url && env.chave ? OK : ERRO, 'Site (2-site/.env)', env.url ? env.url : 'ainda não configurado')
  linhaRelatorio(sec.url && sec.chave ? OK : ERRO, 'Aparelho (3-aparelho/.../secrets.h)',
    sec.url ? `${sec.nodeId || '?'} · envio ${sec.enviarPadrao ? 'ligado' : 'desligado'} ao iniciar` : 'ainda não configurado')

  if (env.url && sec.url && (env.url !== sec.url || env.chave !== sec.chave)) {
    linhaRelatorio(AVISO, 'O site e o aparelho usam Supabase DIFERENTES.', 'rode a configuração guiada para alinhar')
  }
  linhaRelatorio(fs.existsSync(path.join(PASTA_SITE, 'node_modules')) ? OK : ERRO, 'Dependências do site (npm install)',
    fs.existsSync(path.join(PASTA_SITE, 'node_modules')) ? '' : 'ainda não instaladas')
  linhaRelatorio(fs.existsSync(path.join(PASTA_IA, '.venv')) ? OK : OPC, 'Ambiente Python da IA (4-ia/.venv)',
    fs.existsSync(path.join(PASTA_IA, '.venv')) ? '' : 'opcional')
  return { env, sec }
}

// ---------------------------------------------------------------------------
// Passos do assistente
// ---------------------------------------------------------------------------
async function pedirCredenciais(atual) {
  console.log(`
  No painel do Supabase abra ${negrito('Project Settings → Data API')} e copie:
    • ${negrito('Project URL')}                      (https://xxxx.supabase.co)
    • ${negrito('chave anon / publishable')}         (NUNCA a service_role / secret)
  ${cinza('Ainda não tem projeto? Crie em supabase.com (grátis) e volte aqui.')}
`)
  let url = ''
  let chave = ''
  for (;;) {
    url = normalizarUrl(await perguntar('Project URL', atual.url))
    const e = validarUrl(url)
    if (!e) break
    console.log('  ' + vermelho(e))
    if (entradaEncerrada) return null
  }
  for (;;) {
    chave = await perguntar('Chave anon/publishable', atual.chave)
    const e = validarChave(chave)
    if (!e) break
    console.log('  ' + vermelho(e))
    if (entradaEncerrada) return null
  }
  return { url, chave }
}

async function passoBanco(url, chave) {
  titulo('Passo 1 · Banco de dados (Supabase)')
  const antes = await testarSupabase(url, chave)
  if (antes.estado === 'ok') return true
  if (antes.estado === 'rede' || antes.estado === 'chave') return false

  if (!fs.existsSync(ARQ_SCHEMA)) {
    linhaRelatorio(ERRO, 'Não achei 1-banco/schema.sql')
    return false
  }
  const ref = new URL(url).hostname.split('.')[0]
  const sql = fs.readFileSync(ARQ_SCHEMA, 'utf8')
  const copiou = copiarParaAreaDeTransferencia(sql)
  console.log(`
  Vou criar as tabelas. ${copiou ? 'O SQL já foi ' + negrito('copiado') + ' para a sua área de transferência.' : 'Abra o arquivo 1-banco/schema.sql e copie todo o conteúdo.'}
    1. O painel vai abrir no ${negrito('SQL Editor')} (faça login se pedir).
    2. Cole (Ctrl+V) e clique em ${negrito('Run')}. Deve aparecer "Success".
`)
  if (await confirmar('Abrir o SQL Editor no navegador agora?')) {
    abrirNoNavegador(`https://supabase.com/dashboard/project/${ref}/sql/new`)
  }
  for (let tentativa = 0; tentativa < 3; tentativa++) {
    await perguntar('Quando terminar de rodar o SQL, aperte Enter para eu testar')
    const r = await testarSupabase(url, chave)
    if (r.estado === 'ok') return true
    if (entradaEncerrada) break
    console.log('  ' + cinza('Se aparecer erro "already exists" no painel, as tabelas já existem: tudo bem.'))
  }
  return false
}

async function passoSite(url, chave) {
  titulo('Passo 2 · Site (2-site)')
  gravarEnv(url, chave)
  linhaRelatorio(OK, 'Arquivo 2-site/.env criado.')

  if (!fs.existsSync(path.join(PASTA_SITE, 'node_modules'))) {
    if (await confirmar('Instalar as dependências do site agora (npm install, leva alguns minutos)?')) {
      const r = rodar('npm', ['install'], { cwd: PASTA_SITE, stdio: 'inherit' })
      linhaRelatorio(r.status === 0 ? OK : ERRO, r.status === 0 ? 'Dependências instaladas.' : 'npm install falhou. Veja a mensagem acima.')
    } else {
      linhaRelatorio(AVISO, 'Depois rode:  cd 2-site  e  npm install')
    }
  } else {
    linhaRelatorio(OK, 'Dependências do site já instaladas.')
  }
}

async function passoAparelho(url, chave, atual) {
  titulo('Passo 3 · Aparelho (ESP32)')
  let nodeId = ''
  for (;;) {
    nodeId = await perguntar('Nome deste aparelho (letras/números, sem espaço; "no2" para o segundo)', atual.nodeId || 'no1')
    if (/^[a-zA-Z0-9_-]{1,15}$/.test(nodeId)) break
    console.log('  ' + vermelho('Use de 1 a 15 caracteres: letras, números, _ ou -.'))
    if (entradaEncerrada) return
  }
  const enviarPadrao = await confirmar('Já começar enviando os alertas ao Supabase ao ligar? (recomendado para uso diário)', atual.enviarPadrao)
  gravarSecrets({ url, chave, nodeId, enviarPadrao })
  linhaRelatorio(OK, 'Arquivo 3-aparelho/alerta-sons-ia/secrets.h criado.')
  console.log(`
  ${negrito('Falta só gravar no ESP32 (Arduino IDE):')}
    1. Gerenciar Bibliotecas: instale ${negrito('WiFiManager')} (tzapu), ${negrito('Adafruit NeoPixel')} e ${negrito('Chirale_TensorFLowLite')}.
    2. Gerenciador de Placas: instale ${negrito('esp32')} (Espressif) versão 3.x.
    3. Abra  3-aparelho/alerta-sons-ia/alerta-sons-ia.ino
    4. Ferramentas → Placa: ${negrito('ESP32 Dev Module')}  ·  Partition Scheme: ${negrito('Huge APP (3MB No OTA/1MB SPIFFS)')}
    5. Escolha a Porta COM e clique em Carregar.
    6. No primeiro uso, conecte o celular na rede ${negrito('AlertaSons-Setup')} e escolha o Wi-Fi.
`)
  const ide = acharArduinoIDE()
  if (ide && WIN && (await confirmar('Abrir o projeto na Arduino IDE agora?', false))) {
    rodar('cmd', ['/c', 'start', '""', path.join(PASTA_SKETCH, 'alerta-sons-ia.ino')])
  }
}

async function passoTelegram() {
  titulo('Passo 4 · Telegram (opcional)')
  console.log(`  O app e o LED já funcionam sem isto. O Telegram manda o alerta ao celular mesmo
  com o site fechado. Passo a passo em  ${negrito('1-banco/telegram/README.md')}  (leva uns 5 minutos).
`)
}

async function passoIA(py) {
  titulo('Ambiente da IA (opcional)')
  console.log(`  O modelo já vem treinado. Isto só é preciso se for gravar sons novos e retreinar.
  Guia: ${negrito('4-ia/GUIA-IA.md')}
`)
  if (!py) {
    linhaRelatorio(OPC, 'Python não encontrado. Instale o Python 3.12 (python.org) e rode esta opção de novo.')
    return
  }
  if (fs.existsSync(path.join(PASTA_IA, '.venv'))) {
    linhaRelatorio(OK, 'Ambiente 4-ia/.venv já existe.')
    return
  }
  if (!/Python 3\.(1[0-3])/.test(py.versao)) {
    linhaRelatorio(AVISO, `${py.versao}: o TensorFlow pode não instalar. Recomendado: Python 3.12.`)
  }
  if (!(await confirmar('Criar o ambiente Python e instalar o TensorFlow (download grande, ~500 MB)?', false))) return
  let r = rodar(py.cmd, [...py.pre, '-m', 'venv', '.venv'], { cwd: PASTA_IA, stdio: 'inherit' })
  if (r.status !== 0) return linhaRelatorio(ERRO, 'Não consegui criar o ambiente virtual.')
  const pyVenv = WIN ? path.join(PASTA_IA, '.venv', 'Scripts', 'python.exe') : path.join(PASTA_IA, '.venv', 'bin', 'python')
  r = rodar(`"${pyVenv}"`, ['-m', 'pip', 'install', '-r', 'requirements.txt'], { cwd: PASTA_IA, stdio: 'inherit' })
  linhaRelatorio(r.status === 0 ? OK : ERRO, r.status === 0 ? 'Ambiente da IA pronto.' : 'pip install falhou. Veja a mensagem acima.')
}

function resumoFinal(url) {
  titulo('Pronto!')
  console.log(`  ${negrito('Para abrir o site:')}      cd 2-site   e   npm run dev      →  http://localhost:3000
  ${negrito('Para testar tudo:')}       grave o ESP32, abra o Serial Monitor (115200), digite  s 1  e bata palmas.
  ${negrito('Rodar este assistente de novo:')}  setup.bat  (ele lembra o que já foi feito)
  ${negrito('Dúvidas:')}                README.md  e  docs/MANUAL.md
`)
}

// ---------------------------------------------------------------------------
// Fluxos
// ---------------------------------------------------------------------------
async function configuracaoGuiada(py) {
  const { env, sec } = { env: lerEnv(), sec: lerSecrets() }
  const atual = { url: sec.url || env.url, chave: sec.chave || env.chave, nodeId: sec.nodeId, enviarPadrao: sec.enviarPadrao }

  titulo('Configuração guiada')
  const cred = await pedirCredenciais(atual)
  if (!cred) return
  const bancoOk = await passoBanco(cred.url, cred.chave)
  if (!bancoOk) {
    linhaRelatorio(AVISO, 'O banco ainda não está pronto, mas vou gravar as configurações mesmo assim.')
    linhaRelatorio(AVISO, 'Quando criar as tabelas, use a opção "Testar conexão" do menu.')
  }
  await passoSite(cred.url, cred.chave)
  await passoAparelho(cred.url, cred.chave, atual)
  await passoTelegram()
  if (await confirmar('Preparar também o ambiente de treino da IA (opcional)?', false)) await passoIA(py)
  resumoFinal(cred.url)
}

async function testarConexao() {
  titulo('Testar conexão com o Supabase')
  const env = lerEnv()
  const sec = lerSecrets()
  const url = env.url || sec.url
  const chave = env.chave || sec.chave
  if (!url || !chave) {
    linhaRelatorio(ERRO, 'Nada configurado ainda. Rode a configuração guiada primeiro.')
    return false
  }
  console.log(`  Testando ${url}`)
  return (await testarSupabase(url, chave)).estado === 'ok'
}

// ---------------------------------------------------------------------------
// Editar configurações e bancos salvos (perfis)
// ---------------------------------------------------------------------------
// Os perfis ficam em ".setup-perfis.json" (na raiz, fora do GitHub) e guardam só
// URL + chave de cada banco, para alternar entre eles sem digitar tudo de novo.
const ARQ_PERFIS = path.join(RAIZ, '.setup-perfis.json')

function mascarar(chave) {
  if (!chave) return '(vazia)'
  return chave.length <= 16 ? '***' : `${chave.slice(0, 12)}…${chave.slice(-4)}`
}

function lerPerfis() {
  try {
    return JSON.parse(fs.readFileSync(ARQ_PERFIS, 'utf8'))
  } catch {
    return {}
  }
}

function gravarPerfis(perfis) {
  fs.writeFileSync(ARQ_PERFIS, JSON.stringify(perfis, null, 2))
}

function mostrarConfig() {
  const env = lerEnv()
  const sec = lerSecrets()
  titulo('Configuração atual')
  console.log(`  ${negrito('Site')}  ${cinza('(2-site/.env)')}
      URL:    ${env.url || cinza('(não definida)')}
      Chave:  ${mascarar(env.chave)}
  ${negrito('Aparelho')}  ${cinza('(3-aparelho/alerta-sons-ia/secrets.h)')}
      URL:    ${sec.url || cinza('(não definida)')}
      Chave:  ${mascarar(sec.chave)}
      Nome:   ${sec.nodeId || cinza('(não definido)')}
      Envio ao Supabase ao ligar:  ${sec.nodeId || sec.url ? (sec.enviarPadrao ? 'ligado' : 'desligado') : cinza('-')}`)
  if (env.url && sec.url && (env.url !== sec.url || env.chave !== sec.chave)) {
    linhaRelatorio(AVISO, 'O site e o aparelho usam bancos DIFERENTES.', 'eventos do aparelho não aparecerão no site')
  }
}

// Pergunta onde aplicar uma mudança de banco. Devolve 'ambos', 'site' ou 'aparelho'.
async function escolherAlvo() {
  console.log(`
  Aplicar em:
    ${negrito('1')}  Site e aparelho ${cinza('(recomendado: os dois precisam usar o mesmo banco)')}
    ${negrito('2')}  Só no site
    ${negrito('3')}  Só no aparelho`)
  const r = await perguntar('Opção', '1')
  return r === '2' ? 'site' : r === '3' ? 'aparelho' : 'ambos'
}

// Grava URL/chave nos arquivos escolhidos e confere o banco novo.
async function aplicarBanco(url, chave, alvo) {
  if (alvo !== 'aparelho') {
    gravarEnv(url, chave)
    linhaRelatorio(OK, 'Site atualizado (2-site/.env).', 'reinicie o "npm run dev" para valer')
  }
  if (alvo !== 'site') {
    const sec = lerSecrets()
    gravarSecrets({ url, chave, nodeId: sec.nodeId || 'no1', enviarPadrao: sec.enviarPadrao })
    linhaRelatorio(OK, 'Aparelho atualizado (secrets.h).', 'GRAVE O ESP32 DE NOVO na Arduino IDE para valer')
  }
  await passoBanco(url, chave)
}

async function oferecerSalvarPerfil(url, chave) {
  const nome = await perguntar('Salvar este banco com um nome para usar depois? (ex.: antigo, novo; Enter = não salvar)')
  if (!nome) return
  if (!/^[\w -]{1,30}$/.test(nome)) return linhaRelatorio(ERRO, 'Nome inválido: use até 30 letras, números, espaço, _ ou -.')
  const perfis = lerPerfis()
  perfis[nome] = { url, chave }
  gravarPerfis(perfis)
  linhaRelatorio(OK, `Banco salvo como "${nome}".`)
}

async function escolherPerfil(perfis, texto) {
  const nomes = Object.keys(perfis)
  if (!nomes.length) {
    linhaRelatorio(AVISO, 'Nenhum banco salvo ainda.', 'use as opções 1 e 5 para salvar')
    return null
  }
  nomes.forEach((n, i) => console.log(`    ${negrito(String(i + 1))}  ${n}  ${cinza(perfis[n].url)}`))
  const r = await perguntar(texto)
  return nomes.find((n, i) => n === r || String(i + 1) === r) || null
}

async function editarConfig() {
  for (;;) {
    mostrarConfig()
    const perfis = lerPerfis()
    const nomes = Object.keys(perfis)
    console.log(`
${negrito('O que você quer editar?')}
  ${negrito('1')}  Trocar o banco ${cinza('(URL e chave do Supabase)')}
  ${negrito('2')}  Nome do aparelho
  ${negrito('3')}  Envio ao Supabase ao ligar ${cinza('(sim/não)')}
  ${negrito('4')}  Usar um banco salvo ${cinza(nomes.length ? `(${nomes.join(', ')})` : '(nenhum salvo ainda)')}
  ${negrito('5')}  Salvar a configuração atual com um nome ${cinza('(ex.: "antigo", "novo")')}
  ${negrito('6')}  Apagar um banco salvo
  ${negrito('0')}  Voltar
`)
    const op = await perguntar('Opção', '0')
    if (op === '0' || entradaEncerrada) return

    const env = lerEnv()
    const sec = lerSecrets()
    const atual = { url: env.url || sec.url, chave: env.chave || sec.chave }

    if (op === '1') {
      const cred = await pedirCredenciais(atual)
      if (!cred) return
      const alvo = await escolherAlvo()
      await aplicarBanco(cred.url, cred.chave, alvo)
      await oferecerSalvarPerfil(cred.url, cred.chave)
    } else if (op === '2') {
      if (!sec.url) {
        linhaRelatorio(ERRO, 'O aparelho ainda não foi configurado.', 'use a configuração guiada (opção 1 do menu principal)')
      } else {
        let nodeId = ''
        for (;;) {
          nodeId = await perguntar('Novo nome do aparelho (letras/números, sem espaço)', sec.nodeId || 'no1')
          if (/^[a-zA-Z0-9_-]{1,15}$/.test(nodeId)) break
          console.log('  ' + vermelho('Use de 1 a 15 caracteres: letras, números, _ ou -.'))
          if (entradaEncerrada) return
        }
        gravarSecrets({ ...sec, nodeId })
        linhaRelatorio(OK, `Nome do aparelho: ${nodeId}.`, 'GRAVE O ESP32 DE NOVO para valer')
      }
    } else if (op === '3') {
      if (!sec.url) {
        linhaRelatorio(ERRO, 'O aparelho ainda não foi configurado.', 'use a configuração guiada (opção 1 do menu principal)')
      } else {
        const ligado = await confirmar('Já começar enviando os alertas ao Supabase ao ligar?', sec.enviarPadrao)
        gravarSecrets({ ...sec, enviarPadrao: ligado })
        linhaRelatorio(OK, `Envio ao ligar: ${ligado ? 'ligado' : 'desligado'}.`, 'GRAVE O ESP32 DE NOVO para valer')
      }
    } else if (op === '4') {
      const nome = await escolherPerfil(perfis, 'Qual banco usar? (número ou nome)')
      if (nome) {
        const alvo = await escolherAlvo()
        await aplicarBanco(perfis[nome].url, perfis[nome].chave, alvo)
      }
    } else if (op === '5') {
      if (!atual.url || !atual.chave) {
        linhaRelatorio(ERRO, 'Não há configuração para salvar ainda.')
      } else {
        await oferecerSalvarPerfil(atual.url, atual.chave)
      }
    } else if (op === '6') {
      const nome = await escolherPerfil(perfis, 'Qual apagar? (número ou nome)')
      if (nome && (await confirmar(`Apagar o banco salvo "${nome}"? (só some desta lista, o Supabase não é afetado)`, false))) {
        delete perfis[nome]
        gravarPerfis(perfis)
        linhaRelatorio(OK, 'Apagado da lista.')
      }
    }
    if (entradaEncerrada) return
    await perguntar('\nAperte Enter para continuar')
  }
}

async function menu(py) {
  for (;;) {
    mostrarEstadoProjeto()
    console.log(`
${negrito('O que você quer fazer?')}
  ${negrito('1')}  Configuração guiada ${cinza('(recomendado na primeira vez)')}
  ${negrito('2')}  Testar conexão com o Supabase
  ${negrito('3')}  Editar configurações ${cinza('(trocar de banco, nome do aparelho, envio ao ligar)')}
  ${negrito('4')}  Preparar o ambiente da IA (Python) ${cinza('(opcional)')}
  ${negrito('5')}  Ver o que está instalado no computador
  ${negrito('0')}  Sair
`)
    const op = await perguntar('Opção', '1')
    if (op === '1') await configuracaoGuiada(py)
    else if (op === '2') await testarConexao()
    else if (op === '3') await editarConfig()
    else if (op === '4') await passoIA(py)
    else if (op === '5') mostrarRequisitos()
    else if (op === '0' || entradaEncerrada) break
    if (entradaEncerrada) break
    await perguntar('\nAperte Enter para voltar ao menu')
  }
}

// ---------------------------------------------------------------------------
async function main() {
  console.log(negrito(ciano('\n  🔊  ALERTA DE SONS · assistente de configuração')))
  console.log(cinza('  Cria os arquivos de configuração do site e do aparelho e confere o banco.\n'))

  if (!fs.existsSync(ARQ_SCHEMA) || !fs.existsSync(PASTA_SITE)) {
    console.log(vermelho('  Rode este arquivo dentro da pasta do projeto (onde estão 1-banco, 2-site...).'))
    process.exitCode = 1
    return
  }

  if (ARGS.includes('--verificar')) {
    mostrarRequisitos()
    mostrarEstadoProjeto()
    return
  }
  if (ARGS.includes('--testar')) {
    process.exitCode = (await testarConexao()) ? 0 : 1
    return
  }

  const { py } = mostrarRequisitos()
  if (Number(process.versions.node.split('.')[0]) < 22) {
    console.log('\n  ' + amarelo('Atualize o Node.js para a versão 22+ antes de rodar o site (https://nodejs.org).'))
  }
  await menu(py)
  console.log('\n  Até logo!\n')
}

main().finally(() => rl.close())
