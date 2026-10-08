// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// generate-llms.mjs — deterministic generator for the machine-readable surfaces
// that AI agents and crawlers read: public/llms.txt, public/llms-full.txt and
// public/.well-known/ai-plugin.json.
//
//   docs collection (src/content/docs/*.md|mdx, via src/lib/docs-markdown.mjs)
//     + release manifests (src/content/releases-*/release.yaml)
//     + facts (src/data/product-facts.mjs)
//     + the prose and page list in src/data/agent-files.mjs
//     => public/llms.txt       the llms.txt convention: H1, summary, about,
//                              then every docs page grouped as the sidebar
//                              groups them, the tools, the download/pricing/
//                              support pages and the distribution channels
//     => public/llms-full.txt  the same index followed by the Markdown of
//                              every docs page (the text /docs/<slug>.md serves)
//
//   ai-plugin.json.tmpl (scripts/llms-templates/) + LLMS_TOKENS
//     => public/.well-known/ai-plugin.json
//
// Runs at prebuild (see package.json "prebuild") so the published files always
// match the single sources of truth. The byte-equivalence drift guard in
// test/sync/llms-generated.test.ts imports generateContent() from here and
// fails CI if the committed files differ from a fresh render — a docs page or a
// fact changed without regenerating (`npm run gen:llms`), or a hand edit.
//
// Output is deterministic: a stable order (sidebar order; the manifests' own
// field order), no timestamps beyond the manifests' release dates.
//
// Usage:
//   node scripts/generate-llms.mjs            # write public/llms*.txt + the manifest
//   node scripts/generate-llms.mjs --check    # exit 1 if committed files are stale (no write)

import { readFileSync, writeFileSync } from 'node:fs';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { dirname, resolve } from 'node:path';
import {
  LLMS_TOKENS,
  PRODUCT_NAME,
  CURRENT_LINE,
  V1_LINE,
  PKG_APACHE_MODULE,
  PKG_NGINX_MODULE,
  PKG_OPTIMIZER,
  PKG_ASPNETCORE,
  PKG_SIDECAR,
  PKG_SIDECAR_NATIVE,
  SIDECAR_VERSION,
  SIDECAR_NGINX_VERSION,
  SIDECAR_RIDS,
  ASPNETCORE_RIDS,
  ASPNETCORE_TFMS,
  NGINX_APT_MATRIX,
  NGINX_APT_ARCHES,
  STOCK_NGINX_ALMA,
} from '../src/data/product-facts.mjs';
import { SUMMARY, ABOUT, FILTERS_PAGE, SURFACES } from '../src/data/agent-files.mjs';
import {
  SITE,
  loadDocs,
  groupDocs,
  loadManifests,
  renderDocMarkdown,
  dockerTag,
  artifactUrl,
  substituteRelease,
  docUrl,
} from '../src/lib/docs-markdown.mjs';
import { loadFilterTopics } from '../src/lib/filter-topics.mjs';

const __dirname = dirname(fileURLToPath(import.meta.url));
export const WEBSITE_ROOT = resolve(__dirname, '..');
export const TEMPLATE_DIR = resolve(__dirname, 'llms-templates');
export const PUBLIC_DIR = resolve(__dirname, '..', 'public');

/**
 * What gets written. The two text files are built from the collection; the
 * AI-plugin manifest is JSON and still goes through the template+token path
 * (only the drift-prone version/price/variant facts are tokenized; token values
 * never contain double quotes or backslashes — see the PRICING_TIERS note rule
 * in product-facts.mjs — so substitution into a JSON string is safe; the drift
 * test JSON.parses the result, which would catch a violation).
 * @type {Array<{ output: string, build?: (ctx: Context) => string, template?: string }>}
 */
export const TARGETS = [
  { output: 'llms.txt', build: (ctx) => withNotice(buildIndex(ctx)) },
  { output: 'llms-full.txt', build: (ctx) => withNotice(buildFull(ctx)) },
  { template: 'ai-plugin.json.tmpl', output: '.well-known/ai-plugin.json' },
];

const UNRESOLVED_TOKEN = /\{\{\s*[A-Za-z0-9_]+\s*\}\}/;

/**
 * Substitute every {{TOKEN}} in `template` with its value from LLMS_TOKENS.
 * Throws if any {{TOKEN}} remains unresolved (typo or missing fact) so a bad
 * token can never silently ship into a public file.
 * @param {string} template
 * @param {string} name  source name, for error messages
 * @returns {string}
 */
export function render(template, name) {
  let out = template;
  for (const [token, value] of Object.entries(LLMS_TOKENS)) {
    out = out.replaceAll(`{{${token}}}`, value);
  }
  const leftover = out.match(UNRESOLVED_TOKEN);
  if (leftover) {
    throw new Error(
      `${name}: unresolved template token ${leftover[0]} — add it to LLMS_TOKENS ` +
        `in src/data/product-facts.mjs (or fix the typo in the source).`,
    );
  }
  return out;
}

/**
 * @typedef {{
 *   docs: import('../src/lib/docs-markdown.mjs').Doc[],
 *   groups: import('../src/lib/docs-markdown.mjs').DocGroup[],
 *   manifests: import('../src/lib/docs-markdown.mjs').Manifests,
 *   topics: import('../src/lib/filter-topics.mjs').Topic[],
 * }} Context
 */

/**
 * Everything the builders read, loaded once.
 * @param {string} [root]
 * @returns {Context}
 */
export function loadContext(root = WEBSITE_ROOT) {
  const docs = loadDocs(root);
  // Only the indexable filter topic pages are listed; the thin ones are noindex
  // until their group-page section is enriched (src/lib/filter-topics.mjs).
  const topics = loadFilterTopics(root).filter((t) => t.indexable);
  return { docs, groups: groupDocs(docs), manifests: loadManifests(root), topics };
}

/**
 * One index entry, the llms.txt line form.
 * @param {string} title
 * @param {string} url
 * @param {string} description
 */
function entry(title, url, description) {
  return `- [${title}](${url}): ${description}`;
}

/**
 * The distribution channels and packages, every name and version taken from
 * the release manifests and product-facts.mjs.
 * @param {Context} ctx
 * @returns {string[]}
 */
export function distributionLines(ctx) {
  const rel21 = ctx.manifests['2.1'];
  const rel20 = ctx.manifests['2.0'];
  const rel11 = ctx.manifests['1.1'];
  const repo = rel21.urls.archive_base;
  const tag = dockerTag(rel21);
  const aspnetPkg = rel20.artifacts.nuget?.package ?? PKG_ASPNETCORE;
  const current = `${PRODUCT_NAME} ${rel21.release.semver} (${rel21.release.released_on})`;
  return [
    `- Signed apt and yum repository at ${repo} (GPG key ${rel21.urls.gpg_key}; one-time setup \`curl -fsSL ${repo}install.sh | sudo sh\`): \`${PKG_APACHE_MODULE}\` (the Apache module), \`${PKG_NGINX_MODULE}\` (the nginx module, prebuilt and pinned to each distribution's stock nginx: ${NGINX_APT_MATRIX}, ${NGINX_APT_ARCHES}; AlmaLinux 9 via yum against stock nginx ${STOCK_NGINX_ALMA}) and \`${PKG_OPTIMIZER}\` (the optimizer worker). Current release: ${current}. See [the apt/yum repository](${SITE}/download/apt-yum/) and [Install the module on Apache and nginx](${docUrl('installation-module')}).`,
    `- Container images on GHCR: \`${rel21.artifacts.docker.image}:${tag}\` (nginx with the module) and \`${rel21.artifacts.docker.worker_image}:${tag}\` (the optimizer worker), on ${rel21.compat.docker.base_images.join(', ')}. See [Install with Docker](${docUrl('installation-docker')}).`,
    `- Helm chart \`${rel21.artifacts.helm.chart}\` from ${rel21.artifacts.helm.repository} (application version ${substituteRelease(rel21.artifacts.helm.app_version, rel21)}; Kubernetes ${rel21.compat.kubernetes.versions.join(', ')}; Helm ${rel21.compat.kubernetes.helm_versions.join(', ')}). See [Deploy with Helm](${docUrl('helm-deployment')}).`,
    `- IIS: the signed Windows installer from the ${V1_LINE} module channel, ${artifactUrl(rel11, 'msi', 'win_x64')} (${rel11.compat.iis.os.join(', ')}; IIS ${rel11.compat.iis.iis_versions.join(', ')}; x64). See [Install on IIS](${docUrl('install-iis')}).`,
    `- NuGet \`${aspnetPkg}\` ${rel21.release.semver}, the ASP.NET Core middleware (${ASPNETCORE_TFMS.join(', ')}; ${ASPNETCORE_RIDS.join(', ')}): \`dotnet add package ${aspnetPkg}\`. See [Install ASP.NET Core middleware](${docUrl('aspnet-getting-started')}).`,
    `- NuGet \`${PKG_SIDECAR}\` with \`${PKG_SIDECAR_NATIVE}\`, ${SIDECAR_VERSION}, the module as an ASP.NET Core sidecar (${SIDECAR_RIDS.join(', ')}; bundles nginx ${SIDECAR_NGINX_VERSION}): \`dotnet add package ${PKG_SIDECAR}\`.`,
    `- Direct downloads of the ${V1_LINE} module line (deb, rpm and MSI, with SHA256SUMS and GPG signatures) at ${rel11.urls.archive_base}: ${rel11.release.tag} (${rel11.release.released_on}). See [Download](${SITE}/download/).`,
  ];
}

/**
 * The index: H1, summary, about, then the link sections.
 * @param {Context} ctx
 * @returns {string}
 */
export function buildIndex(ctx) {
  /** @type {string[]} */
  const lines = [
    `# ${PRODUCT_NAME} ${CURRENT_LINE}`,
    '',
    `> ${render(SUMMARY, 'agent-files.mjs SUMMARY')}`,
    '',
    render(ABOUT, 'agent-files.mjs ABOUT'),
    '',
  ];
  for (const group of ctx.groups) {
    lines.push(`## ${group.label}`, '');
    for (const doc of group.docs) {
      lines.push(entry(doc.data.title, docUrl(doc.slug), doc.data.description));
    }
    if (group.label === 'Filters') {
      for (const t of ctx.topics) {
        lines.push(
          entry(`${t.human} (${t.name})`, `${SITE}${t.path}`, t.summary.replace(/\.?$/, '.')),
        );
      }
    }
    if (group.label === 'Reference') {
      lines.push(
        entry(FILTERS_PAGE.title, `${SITE}${FILTERS_PAGE.path}`, FILTERS_PAGE.description),
      );
    }
    lines.push('');
  }
  for (const { section, items } of SURFACES) {
    lines.push(`## ${section}`, '');
    for (const item of items) {
      lines.push(
        entry(
          render(item.title, 'agent-files.mjs SURFACES'),
          `${SITE}${item.path}`,
          render(item.description, 'agent-files.mjs SURFACES'),
        ),
      );
    }
    lines.push('');
  }
  lines.push('## Distribution', '', ...distributionLines(ctx), '');
  return lines.join('\n');
}

/**
 * The index followed by every page's Markdown, in sidebar order.
 * @param {Context} ctx
 * @returns {string}
 */
export function buildFull(ctx) {
  const parts = [buildIndex(ctx).trimEnd(), ''];
  for (const doc of ctx.docs) {
    parts.push('---', '', renderDocMarkdown(doc, ctx.manifests).trimEnd(), '');
  }
  return parts.join('\n');
}

/** @param {string} text */
function withNotice(text) {
  return (
    text.trimEnd() +
    '\n\n<!-- Generated by scripts/generate-llms.mjs from the docs collection, the release manifests and src/data/product-facts.mjs; regenerate with `npm run gen:llms`, never edit by hand. -->\n'
  );
}

/**
 * Render every target. Returns { outputFilename: content }.
 * This is the single render path shared by the CLI and the drift-guard test.
 * @param {string} [root]
 * @returns {Record<string, string>}
 */
export function generateContent(root = WEBSITE_ROOT) {
  const ctx = loadContext(root);
  /** @type {Record<string, string>} */
  const result = {};
  for (const target of TARGETS) {
    if (target.build) {
      result[target.output] = target.build(ctx);
    } else if (target.template) {
      const tmpl = readFileSync(resolve(TEMPLATE_DIR, target.template), 'utf8');
      result[target.output] = render(tmpl, target.template);
    }
  }
  return result;
}

function main() {
  const checkOnly = process.argv.includes('--check');
  const content = generateContent();
  let stale = 0;

  for (const { output } of TARGETS) {
    const outPath = resolve(PUBLIC_DIR, output);
    const rendered = content[output];

    if (checkOnly) {
      let current = '';
      try {
        current = readFileSync(outPath, 'utf8');
      } catch {
        /* missing file counts as stale */
      }
      if (current !== rendered) {
        stale++;
        console.error(
          `STALE: public/${output} differs from generated output. Run \`npm run gen:llms\`.`,
        );
      } else {
        console.log(`OK: public/${output} is up to date.`);
      }
    } else {
      writeFileSync(outPath, rendered);
      console.log(`Generated public/${output} (${rendered.length} bytes).`);
    }
  }

  if (checkOnly && stale > 0) {
    process.exit(1);
  }
}

// Run the CLI only when invoked directly (`node scripts/generate-llms.mjs`),
// not when imported by the drift-guard test.
if (import.meta.url === pathToFileURL(process.argv[1] || '').href) {
  main();
}
