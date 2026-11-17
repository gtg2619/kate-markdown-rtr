import {mathjax} from 'mathjax-full/js/mathjax.js';
import {TeX} from 'mathjax-full/js/input/tex.js';
import {SVG} from 'mathjax-full/js/output/svg.js';
import {liteAdaptor} from 'mathjax-full/js/adaptors/liteAdaptor.js';
import {RegisterHTMLHandler} from 'mathjax-full/js/handlers/html.js';
import 'mathjax-full/js/input/tex/base/BaseConfiguration.js';
import 'mathjax-full/js/input/tex/ams/AmsConfiguration.js';
import 'mathjax-full/js/input/tex/newcommand/NewcommandConfiguration.js';
import 'mathjax-full/js/input/tex/boldsymbol/BoldsymbolConfiguration.js';
import 'mathjax-full/js/input/tex/mathtools/MathtoolsConfiguration.js';

const adaptor = liteAdaptor();
RegisterHTMLHandler(adaptor);
export function render(source, display) {
  // Fresh input state for each formula: macros, counters and labels cannot leak.
  const tex = new TeX({packages: ['base', 'ams', 'newcommand', 'boldsymbol', 'mathtools'],
    maxBuffer: 8192, maxMacros: 500, tags: 'none',
    formatError: (_jax, error) => { throw error; }});
  const output = new SVG({fontCache: 'none'});
  const document = mathjax.document('', {InputJax: tex, OutputJax: output});
  try {
    const node = document.convert(source, {display: !!display, em: 16, ex: 8, containerWidth: 100000});
    const svg = adaptor.tags(node, 'svg')[0];
    const width = parseFloat(adaptor.getAttribute(svg, 'width'));
    const height = parseFloat(adaptor.getAttribute(svg, 'height'));
    const depth = -parseFloat(adaptor.getStyle(svg, 'vertical-align') || '0');
    return {svg: adaptor.outerHTML(svg), width, height, depth, error: ''};
  } catch (error) {
    return {svg: '', width: 0, height: 0, depth: 0, error: String(error.message || error).slice(0, 240)};
  }
}
