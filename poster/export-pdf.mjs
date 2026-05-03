import puppeteer from 'puppeteer';
import { fileURLToPath } from 'url';
import { dirname, join } from 'path';

const __dirname = dirname(fileURLToPath(import.meta.url));
const htmlPath = join(__dirname, 'new-poster.html');
const pdfPath  = join(__dirname, 'MycoFlow_Poster_A1.pdf');

// Poster native size (CSS px)
const POSTER_W = 1188, POSTER_H = 1680;

// At 96 DPI: 1px = 25.4/96 mm = 0.2646 mm
// 1188px = 314.33 mm  →  zoom to fill A1 (594 mm): 594 / 314.33 = 1.8897
const ZOOM = 594 / (POSTER_W * 25.4 / 96);  // ≈ 1.8897

const browser = await puppeteer.launch({
  executablePath: '/snap/bin/chromium',
  args: ['--no-sandbox', '--disable-setuid-sandbox'],
  headless: true,
});

const page = await browser.newPage();

// Match viewport to poster so JS scale() computes ≈ 1.0
await page.setViewport({ width: POSTER_W, height: POSTER_H, deviceScaleFactor: 1 });

await page.goto(`file://${htmlPath}`, { waitUntil: 'networkidle0', timeout: 30000 });
await page.evaluateHandle('document.fonts.ready');

// Cancel JS scale, flatten stage, apply zoom on body to fill A1 width
await page.evaluate((zoom, pw, ph) => {
  const poster = document.getElementById('poster');
  const stage  = document.getElementById('stage');

  // Remove JS-set transform and shadow
  poster.style.transform = 'none';
  poster.style.boxShadow = 'none';

  // Flat block wrapper — exact poster dimensions
  stage.style.cssText = `display:block;width:${pw}px;height:${ph}px;overflow:hidden;`;

  // CSS zoom scales the entire body layout
  document.body.style.cssText = `margin:0;padding:0;background:white;display:block;zoom:${zoom};`;

  const ctrl = document.querySelector('.export-controls');
  if (ctrl) ctrl.style.display = 'none';
}, ZOOM, POSTER_W, POSTER_H);

// Give browser two rAF ticks to repaint before capture
await page.evaluate(() => new Promise(r => requestAnimationFrame(() => requestAnimationFrame(r))));

await page.pdf({
  path: pdfPath,
  width:  '594mm',
  height: '840mm',   // 1680px × zoom = exactly 840mm; avoids bottom gap
  printBackground: true,
  margin: { top: 0, right: 0, bottom: 0, left: 0 },
  pageRanges: '1',
});

await browser.close();
console.log('✅  PDF exported:', pdfPath);
