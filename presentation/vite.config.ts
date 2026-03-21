import { defineConfig } from 'vite'

export default defineConfig({
  base: process.env.SLIDEV_EXPORT ? '/' : '/presentation/agentfs/',
})
