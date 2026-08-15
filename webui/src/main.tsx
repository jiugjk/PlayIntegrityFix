import { StrictMode } from 'react'
import { createRoot } from 'react-dom/client'
import '@material/web/button/filled-button.js'
import '@material/web/button/outlined-button.js'
import '@material/web/button/text-button.js'
import '@material/web/chips/assist-chip.js'
import '@material/web/chips/chip-set.js'
import '@material/web/chips/filter-chip.js'
import '@material/web/dialog/dialog.js'
import '@material/web/divider/divider.js'
import '@material/web/icon/icon.js'
import '@material/web/iconbutton/icon-button.js'
import '@material/web/iconbutton/outlined-icon-button.js'
import '@material/web/radio/radio.js'
import '@material/web/ripple/ripple.js'
import '@material/web/switch/switch.js'
import './index.css'
import App from './App.tsx'

createRoot(document.getElementById('root')!).render(
  <StrictMode>
    <App />
  </StrictMode>,
)
