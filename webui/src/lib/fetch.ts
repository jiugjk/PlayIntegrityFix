/**
 * Try an array of URLs in order, return the first successful Response.
 * @param urls Array of URLs to try
 * @returns Response object
 */
export async function fallbackFetch(urls: string[]): Promise<Response> {
  for (const url of urls) {
    try {
      const res = await fetch(url, { signal: AbortSignal.timeout(15000) })
      if (res.ok) return res
    } catch (e) {
      console.warn(`Failed to fetch ${url}:`, e)
    }
  }
  throw new Error(`All ${urls.length} fetch URLs failed`)
}
