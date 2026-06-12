import { createConnection, ProposedFeatures, Diagnostic, DiagnosticSeverity } from "vscode-languageserver/node";
import * as fs from "fs";
import * as path from "path";
import { pathToFileURL, fileURLToPath } from "url";

interface ReportEntry {
  file: string;
  line: number;
  col?: number;
  severity?: string;
  message: string;
  rank?: number;
  peer?: number;
  type?: string;
  peer_file?: string;
  peer_line?: number;
}

interface ReportFile {
  errors: ReportEntry[];
}

const connection = createConnection(ProposedFeatures.all);

let rootPath = "";
let reportPath = "";
let watcher: fs.FSWatcher | undefined;
let debounce: NodeJS.Timeout | undefined;
let lastUris: Set<string> = new Set();

connection.onInitialize((params) => {
  rootPath = params.rootUri ? path.resolve(fileURLToPath(params.rootUri)) : process.cwd();
  reportPath = params.initializationOptions?.reportPath || path.join(rootPath, "mpi_report.json");
  startWatch();
  return {
    capabilities: {},
  };
});

function startWatch(): void {
  stopWatch();
  scheduleRead();
  try {
    watcher = fs.watch(reportPath, { persistent: true }, () => scheduleRead());
  } catch {
    // Ignore if report is missing.
  }
}

function stopWatch(): void {
  if (watcher) {
    watcher.close();
    watcher = undefined;
  }
  if (debounce) {
    clearTimeout(debounce);
    debounce = undefined;
  }
}

function scheduleRead(): void {
  if (debounce) {
    clearTimeout(debounce);
  }
  debounce = setTimeout(() => {
    readAndPublish().catch(() => clearAllDiagnostics());
  }, 120);
}

function clearAllDiagnostics(): void {
  for (const uri of lastUris) {
    connection.sendDiagnostics({ uri, diagnostics: [] });
  }
  lastUris = new Set();
}

async function readAndPublish(): Promise<void> {
  const raw = await fs.promises.readFile(reportPath, "utf8");
  const report = JSON.parse(raw) as ReportFile;
  const errors = Array.isArray(report?.errors) ? report.errors : [];

  const byFile = new Map<string, Diagnostic[]>();
  for (const e of errors) {
    if (!e.file || !e.message || !e.line) {
      continue;
    }
    let file = e.file;
    if (process.platform === "win32" && file.startsWith("/mnt/")) {
      const drive = file.charAt(5);
      file = drive + ":" + file.slice(6).replace(/\//g, "\\");
    }
    const abs = path.isAbsolute(file) ? file : path.resolve(rootPath, file);
    const uri = pathToFileURL(abs).toString();

    const startLine = Math.max(0, (e.line || 1) - 1);
    const startCol = Math.max(0, e.col || 0);
    const diag: Diagnostic = {
      range: {
        start: { line: startLine, character: startCol },
        end: { line: startLine, character: 1000 },
      },
      severity: mapSeverity(e.severity),
      message: `[rank ${e.rank ?? "?"}${e.peer !== undefined ? `\u2192${e.peer}` : ""}] ${e.message}`,
      source: "mpi-sanitizer",
      code: e.type,
    };

    if (!byFile.has(uri)) {
      byFile.set(uri, []);
    }
    byFile.get(uri)?.push(diag);
  }

  for (const [uri, diagnostics] of byFile.entries()) {
    connection.sendDiagnostics({ uri, diagnostics });
  }

  const nextUris = new Set(byFile.keys());
  for (const uri of lastUris) {
    if (!nextUris.has(uri)) {
      connection.sendDiagnostics({ uri, diagnostics: [] });
    }
  }
  lastUris = nextUris;
}

function mapSeverity(s?: string): DiagnosticSeverity {
  switch ((s || "error").toLowerCase()) {
    case "warning":
      return DiagnosticSeverity.Warning;
    case "info":
      return DiagnosticSeverity.Information;
    default:
      return DiagnosticSeverity.Error;
  }
}

connection.listen();
