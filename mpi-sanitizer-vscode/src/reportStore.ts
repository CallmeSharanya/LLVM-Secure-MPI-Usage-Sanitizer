import * as fs from "fs";
import * as path from "path";
import * as vscode from "vscode";

export type MpiSeverity = "error" | "warning" | "info";

export interface MpiReportEntry {
  file: string;
  line: number;
  col?: number;
  severity?: MpiSeverity;
  message: string;
  rank?: number;
  peer?: number;
  type?: string;
  peer_file?: string;
  peer_line?: number;
}

export interface MpiReport {
  errors: MpiReportEntry[];
}

export class ReportStore {
  private report: MpiReport = { errors: [] };
  private readonly emitter = new vscode.EventEmitter<MpiReport>();
  private watcher?: fs.FSWatcher;
  private debounce?: NodeJS.Timeout;

  constructor(private readonly root: string, private readonly reportPath: string) {}

  onDidUpdate = this.emitter.event;

  getReport(): MpiReport {
    return this.report;
  }

  getEntriesForFileLine(file: string, line1: number): MpiReportEntry[] {
    const abs = path.isAbsolute(file) ? file : path.resolve(this.root, file);
    return this.report.errors.filter((e) => {
      const entryAbs = path.isAbsolute(e.file) ? e.file : path.resolve(this.root, e.file);
      return entryAbs === abs && e.line === line1;
    });
  }

  startWatching(): void {
    this.stopWatching();
    this.scheduleLoad();

    try {
      this.watcher = fs.watch(this.reportPath, { persistent: true }, () => {
        this.scheduleLoad();
      });
    } catch {
      // If the file does not exist yet, still attempt periodic reloads on demand.
    }
  }

  stopWatching(): void {
    if (this.watcher) {
      this.watcher.close();
      this.watcher = undefined;
    }
    if (this.debounce) {
      clearTimeout(this.debounce);
      this.debounce = undefined;
    }
  }

  private scheduleLoad(): void {
    if (this.debounce) {
      clearTimeout(this.debounce);
    }
    this.debounce = setTimeout(() => {
      this.loadReport().catch(() => {
        this.report = { errors: [] };
        this.emitter.fire(this.report);
      });
    }, 150);
  }

  private async loadReport(): Promise<void> {
    const raw = await fs.promises.readFile(this.reportPath, "utf8");
    const json: { errors?: unknown } = JSON.parse(raw);
    const errors = Array.isArray(json.errors) ? json.errors : [];

    const sanitized: MpiReportEntry[] = errors
      .filter((e: unknown) => typeof e === "object" && e !== null)
      .map((entry) => {
        const e = entry as Record<string, unknown>;
        return {
          file: String(e.file || ""),
          line: Number(e.line || 0),
          col: e.col !== undefined ? Number(e.col) : 0,
          severity: (e.severity || "error") as MpiSeverity,
          message: String(e.message || ""),
          rank: e.rank !== undefined ? Number(e.rank) : undefined,
          peer: e.peer !== undefined ? Number(e.peer) : undefined,
          type: e.type !== undefined ? String(e.type) : undefined,
          peer_file: e.peer_file !== undefined ? String(e.peer_file) : undefined,
          peer_line: e.peer_line !== undefined ? Number(e.peer_line) : undefined,
        };
      })
      .filter((e) => e.file && e.line > 0 && e.message);

    this.report = { errors: sanitized };
    this.emitter.fire(this.report);
  }
}