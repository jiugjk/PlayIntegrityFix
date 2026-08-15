import { exec, spawn, toast } from 'kernelsu-alt'
import {
  AUTO_SECURITY_PATCH_FLAG,
  MODDIR,
  PIF_PROP_CUSTOM_PATH,
  PIF_PROP_DEFAULT_PATH,
  ROOT_BIN_PATH,
  SCRIPT_ONLY_FLAG,
} from '../constant'

const ROOT_ENV = { PATH: ROOT_BIN_PATH }

export interface InitState {
  version: string
  tampered: boolean
  trickyStoreSupported: boolean
  autoSecurityPatch: boolean
  scriptOnly: boolean
  selinux: string
  romSignature: string
  propOutdated: boolean
  pifContent: string
  pifPath: string
}

function parseInitState(stdout: string): InitState {
  const marker = stdout.indexOf('__PIF__')
  const header = marker >= 0 ? stdout.slice(0, marker) : stdout
  let pifContent = ''
  if (marker >= 0) {
    const body = stdout.slice(marker + '__PIF__'.length).replace(/^\r?\n/, '')
    const end = body.lastIndexOf('__END__')
    pifContent = (end >= 0 ? body.slice(0, end) : body).replace(/\r?\n$/, '')
  }

  const fields: Record<string, string> = {}
  for (const line of header.split(/\r?\n/)) {
    const eq = line.indexOf('=')
    if (eq <= 0) continue
    fields[line.slice(0, eq)] = line.slice(eq + 1)
  }

  return {
    version: fields.VERSION ?? '',
    tampered: fields.TAMPERED === '1',
    trickyStoreSupported: fields.TS_SUPPORTED === '1',
    autoSecurityPatch: fields.AUTO_PATCH === '1',
    scriptOnly: fields.SCRIPT_ONLY === '1',
    selinux: fields.SELINUX ?? '',
    romSignature: fields.ROM_SIG ?? '',
    propOutdated: fields.PROP_OUTDATED === '1',
    pifContent,
    pifPath: fields.PIF_PATH ?? '',
  }
}

export class Cli {
  static killGms(): void {
    void exec(
      'pids="$(busybox pidof com.google.android.gms.unstable com.android.vending 2>/dev/null || pidof com.google.android.gms.unstable com.android.vending 2>/dev/null)" && [ -n "$pids" ] && kill -9 $pids',
      { env: ROOT_ENV },
    )
  }

  static async loadInitState(): Promise<InitState> {
    const { errno, stdout, stderr } = await exec(
      `
      echo "VERSION=$(grep '^version=' "${MODDIR}/module.prop" 2>/dev/null | cut -d= -f2)"
      if grep -q 'tampered' "${MODDIR}/module.prop" 2>/dev/null; then echo TAMPERED=1; else echo TAMPERED=0; fi
      if [ -d /data/adb/modules/tricky_store ] && [ ! -f /data/adb/modules/tricky_store/disable ]; then
        echo TS_SUPPORTED=1
      else
        echo TS_SUPPORTED=0
      fi
      if [ -e "${AUTO_SECURITY_PATCH_FLAG}" ]; then echo AUTO_PATCH=1; else echo AUTO_PATCH=0; fi
      if [ -e "${SCRIPT_ONLY_FLAG}" ]; then echo SCRIPT_ONLY=1; else echo SCRIPT_ONLY=0; fi
      echo "SELINUX=$(getenforce 2>/dev/null)"
      echo "ROM_SIG=$(unzip -l /system/etc/security/otacerts.zip 2>/dev/null | grep -oE 'testkey|releasekey' | head -n1)"
      pif_file=""
      if [ -f "${PIF_PROP_CUSTOM_PATH}" ]; then pif_file="${PIF_PROP_CUSTOM_PATH}"
      elif [ -f "${PIF_PROP_DEFAULT_PATH}" ]; then pif_file="${PIF_PROP_DEFAULT_PATH}"
      fi
      echo "PIF_PATH=$pif_file"
      outdated=0
      if [ -n "$pif_file" ]; then
        prop_date="$(grep "^SECURITY_PATCH=" "$pif_file" | cut -d= -f2)"
        if [ -n "$prop_date" ]; then
          prop_epoch="$(busybox date -d "$prop_date" +%s 2>/dev/null)"
          current_epoch="$(busybox date +%s)"
          if [ -n "$prop_epoch" ] && [ $((current_epoch - prop_epoch)) -gt 5184000 ]; then
            outdated=1
          fi
        fi
      fi
      echo "PROP_OUTDATED=$outdated"
      echo __PIF__
      if [ -n "$pif_file" ]; then cat "$pif_file"; fi
      echo
      echo __END__
      `,
      { env: ROOT_ENV },
    )
    if (errno !== 0 && !stdout) {
      throw new Error(stderr || `loadInitState failed (${errno})`)
    }
    return parseInitState(stdout)
  }

  static async loadVersion(): Promise<string> {
    const { errno, stdout } = await exec(`grep '^version=' ${MODDIR}/module.prop | cut -d'=' -f2`)
    if (errno === 0) return stdout.trim()
    return ''
  }

  static async checkTampered(): Promise<boolean> {
    const { errno } = await exec(`grep -q 'tampered' ${MODDIR}/module.prop`)
    return errno === 0
  }

  static async checkPropDate(): Promise<string> {
    const result = await exec(
      `
      prop_date="$(grep "^SECURITY_PATCH=" ${PIF_PROP_CUSTOM_PATH} ${PIF_PROP_DEFAULT_PATH} 2>/dev/null | cut -d'=' -f2 | head -n 1)"
      prop_epoch="$(busybox date -d "$prop_date" +%s)"
      current_epoch="$(busybox date +%s)"
      different="$(($current_epoch - $prop_epoch))"
      if [ $different -gt 5184000 ]; then echo "outdated"; fi
    `,
      { env: ROOT_ENV },
    )
    if (result.stdout.includes('outdated')) return 'outdated'
    return ''
  }

  static async checkSELinux(): Promise<string> {
    const { errno, stdout } = await exec('getenforce')
    if (errno !== 0) return ''
    return stdout.trim()
  }

  static async checkRomSignature(): Promise<string> {
    const { errno, stdout } = await exec(
      'unzip -l /system/etc/security/otacerts.zip | grep -oE "testkey|releasekey" | head -n1',
    )
    if (errno !== 0) return ''
    return stdout.trim()
  }

  static runSecurityPatch(): ReturnType<typeof exec> {
    return exec(`sh ${MODDIR}/security_patch.sh`, { env: ROOT_ENV })
  }

  static runAutopifScript(opts?: { env?: Record<string, string> }) {
    return spawn('sh', [`${MODDIR}/autopif.sh`], {
      ...opts,
      env: { ...ROOT_ENV, ...opts?.env },
    })
  }

  static runAutopifOta() {
    return spawn('sh', [`${MODDIR}/autopif_ota.sh`], { env: ROOT_ENV })
  }

  static openLink(url: string) {
    toast(`Redirecting to ${url}`)
    setTimeout(() => {
      exec(`am start -a android.intent.action.VIEW -d "${url}"`)
        .then(({ errno }) => {
          if (errno !== 0) window.open(url, '_blank')
        })
        .catch(() => window.open(url, '_blank'))
    }, 100)
  }

  static async toggleAutoSecurityPatch(enable: boolean): Promise<void> {
    const result = await exec(`sh ${MODDIR}/security_patch.sh --${enable ? 'enable' : 'disable'}`, {
      env: ROOT_ENV,
    })
    if (result.errno !== 0) {
      throw new Error(result.stderr || `toggleAutoSecurityPatch failed (${result.errno})`)
    }
  }
}
