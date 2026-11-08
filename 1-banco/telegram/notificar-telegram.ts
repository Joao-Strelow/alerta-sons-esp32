// Edge Function chamada pelo Database Webhook toda vez que um evento novo
// e inserido na tabela "eventos". Ela busca o chat_id configurado e manda
// o alerta como mensagem no Telegram.
//
// Variaveis de ambiente necessarias (configuradas em Project Settings ->
// Edge Functions -> Secrets, ver README):
// - TELEGRAM_BOT_TOKEN: token do bot, gerado pelo @BotFather
// - SUPABASE_URL / SUPABASE_SERVICE_ROLE_KEY: ja existem por padrao

import { createClient } from 'jsr:@supabase/supabase-js@2'

const TEXTOS: Record<string, string> = {
  campainha: '🔔 Campainha tocou',
  palma: '👏 Palma detectada',
}

Deno.serve(async (req) => {
  const payload = await req.json()
  const evento = payload.record as { tipo: string; node_id: string; created_at: string }

  const sb = createClient(
    Deno.env.get('SUPABASE_URL')!,
    Deno.env.get('SUPABASE_SERVICE_ROLE_KEY')!,
  )

  const { data: config } = await sb
    .from('config')
    .select('telegram_chat_id')
    .eq('id', 1)
    .single()

  if (!config?.telegram_chat_id) {
    return new Response('Sem chat_id configurado, nada a fazer.', { status: 200 })
  }

  const texto = TEXTOS[evento.tipo] ?? `🔊 Som detectado (${evento.tipo})`
  const hora = new Date(evento.created_at).toLocaleTimeString('pt-BR')
  const mensagem = `${texto}\nDispositivo: ${evento.node_id}\nHorario: ${hora}`

  const botToken = Deno.env.get('TELEGRAM_BOT_TOKEN')
  const resp = await fetch(`https://api.telegram.org/bot${botToken}/sendMessage`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ chat_id: config.telegram_chat_id, text: mensagem }),
  })

  return new Response(await resp.text(), { status: resp.ok ? 200 : 500 })
})
