export default defineNuxtConfig({
  modules: ['@vite-pwa/nuxt'],
  app: {
    head: {
      link: [{ rel: 'icon', type: 'image/svg+xml', href: '/favicon.svg' }],
    },
  },
  runtimeConfig: {
    public: {
      supabaseUrl: '',   // preenchido por NUXT_PUBLIC_SUPABASE_URL
      supabaseKey: '',   // preenchido por NUXT_PUBLIC_SUPABASE_KEY
    },
  },
  pwa: {
    registerType: 'autoUpdate',
    manifest: {
      name: 'Alerta de Sons ESP32',
      short_name: 'AlertaESP32',
      theme_color: '#0d1b2a',
      background_color: '#0d1b2a',
      display: 'standalone',
      icons: [
        { src: 'icon-192.png', sizes: '192x192', type: 'image/png' },
        { src: 'icon-512.png', sizes: '512x512', type: 'image/png' },
      ],
    },
  },
})
