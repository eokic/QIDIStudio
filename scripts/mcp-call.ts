#!/usr/bin/env bun
// Calls the MCP server of a running QIDI Studio. Use it for tools that were added after a Claude Code
// session started: Claude Code keeps the tool list from its start. The token and the port come from
// QIDI Studio's settings. The script does not print the token.
//
//   bun scripts/mcp-call.ts list                         Tools, with their arguments.
//   bun scripts/mcp-call.ts <tool> ['<json args>'] ...   Call the tools in this order.
//   bun scripts/mcp-call.ts smoke                        Checks that do not touch a print. Exit 1 on a failure.
//                                                        In an empty project, it also loads and deletes a cone.
//
// Pictures in a reply go to $TMPDIR/qidi-mcp/.

import { tmpdir } from "node:os";

const confPath = `${process.env.HOME}/Library/Application Support/QIDIStudio/QIDIStudio.conf`;
const conf = await Bun.file(confPath).text().catch(() => "{}");
const app = JSON.parse(conf.split("\n").filter((l) => !l.startsWith("#")).join("\n")).app ?? {};
const token: string | undefined = process.env.QIDI_MCP_TOKEN ?? app.mcp_server_token;
const port: string = process.env.QIDI_MCP_PORT ?? app.mcp_server_port ?? "13140";
const picDir = `${tmpdir()}/qidi-mcp`;

function fail(msg: string): never {
  console.error(msg);
  process.exit(1);
}

if (!token) fail("No MCP token. Turn the server on in QIDI Studio > Preferences > AI Assistant (MCP).");

let id = 0;
async function rpc(method: string, params: unknown) {
  const r = await fetch(`http://127.0.0.1:${port}/mcp`, {
    method: "POST",
    headers: { Authorization: `Bearer ${token}`, "Content-Type": "application/json", Accept: "application/json, text/event-stream" },
    body: JSON.stringify({ jsonrpc: "2.0", id: ++id, method, params }),
  }).catch(() => fail(`QIDI Studio does not answer on port ${port}. Start it with scripts/mac-mcp-app.sh restart.`));
  const text = await r.text();
  if (!r.ok) fail(`${method}: HTTP ${r.status} ${text}`);
  const j = JSON.parse(text);
  if (j.error) fail(`${method}: ${JSON.stringify(j.error)}`);
  return j.result;
}

// Calls one tool. Returns the text items as one string, and saves each picture to picDir.
let pics = 0;
async function call(name: string, args: object = {}) {
  const res = await rpc("tools/call", { name, arguments: args });
  const texts: string[] = [];
  for (const c of res.content ?? []) {
    if (c.type === "image") {
      const file = `${picDir}/${name}-${++pics}.${c.mimeType === "image/jpeg" ? "jpg" : "png"}`;
      await Bun.write(file, Buffer.from(c.data, "base64"));
      texts.push(`[picture ${c.mimeType}] ${file}`);
    } else texts.push(c.text ?? JSON.stringify(c));
  }
  return { text: texts.join("\n"), isError: !!res.isError };
}

await rpc("initialize", { protocolVersion: "2025-06-18", capabilities: {}, clientInfo: { name: "mcp-call", version: "1" } });
const argv = process.argv.slice(2);

if (argv[0] === "list") {
  for (const t of (await rpc("tools/list", {})).tools) {
    const req: string[] = t.inputSchema?.required ?? [];
    const props = Object.entries(t.inputSchema?.properties ?? {}).map(([k, v]: [string, any]) => `${k}${req.includes(k) ? "*" : ""}:${v.type ?? "?"}`);
    console.log(`- ${t.name}(${props.join(", ")}) ${JSON.stringify(t.annotations ?? {})}\n    ${t.description}`);
  }
} else if (argv[0] === "smoke") {
  let failed = false;
  const check = (name: string, ok: boolean, detail = "") => {
    console.log(ok ? `ok   ${name}` : `FAIL ${name}${detail ? `: ${detail}` : ""}`);
    if (!ok) failed = true;
  };
  const tools = new Map<string, any>(((await rpc("tools/list", {})).tools as any[]).map((t) => [t.name, t]));
  const want = ["get_status", "load_model", "slice", "send_print", "printer_status", "camera_snapshot", "pause_print", "resume_print", "cancel_print"];
  const missing = want.filter((n) => !tools.has(n));
  check(`tools/list (${tools.size} tools)`, missing.length === 0, missing.length ? `missing ${missing.join(", ")}` : "");
  for (const n of ["send_print", "cancel_print"]) check(`${n} is marked destructive`, tools.get(n)?.annotations?.destructiveHint === true);
  const status = await call("get_status");
  check("get_status", !status.isError && status.text.includes('"app"'), status.isError ? status.text : "");
  // cancel_print checks confirm before it sends anything to the printer.
  const cancel = await call("cancel_print");
  check("cancel_print without confirm is refused", cancel.isError && cancel.text.includes("confirm: true"), cancel.text);
  // A delete that left the object's row in the object list crashed the next load_model.
  // Loads and deletes a cone two times, only in an empty project with no file. In other projects, the check
  // adds 4 undo steps and unsaved changes, and clears the slice result. A crash shows as "does not answer".
  const project = status.isError ? {} : JSON.parse(status.text);
  const empty = project.objects?.length === 0 && !project.project_file;
  if (!empty) console.log(`info load_model / delete_object skipped: ${status.isError ? "get_status failed" : "the project is not empty"}`);
  const cone = `${import.meta.dir}/../resources/model/cone.stl`;
  for (const step of empty ? [1, 2] : []) {
    const load = await call("load_model", { path: cone });
    const idx: number = load.isError ? -1 : JSON.parse(load.text).loaded[0].index;
    check(`load_model ${step}`, idx >= 0, load.text);
    if (idx < 0) break;
    const del = await call("delete_object", { object: idx });
    check(`delete_object ${step}`, !del.isError, del.text);
    if (del.isError) break;
  }
  // Information only: the printer can be off.
  const printer = await call("printer_status");
  console.log(`info printer_status: ${printer.isError ? printer.text : (printer.text.match(/"state": "[a-z]*"/)?.[0] ?? "?")}`);
  process.exit(failed ? 1 : 0);
} else if (argv.length) {
  for (let i = 0; i < argv.length; i++) {
    const name = argv[i];
    const args = argv[i + 1]?.startsWith("{") ? JSON.parse(argv[++i]) : {};
    const r = await call(name, args);
    console.log(`=== ${name} ${JSON.stringify(args)}${r.isError ? " (error)" : ""}\n${r.text}`);
  }
} else {
  console.log("Usage: bun scripts/mcp-call.ts list | smoke | <tool> ['<json args>'] ...");
}
