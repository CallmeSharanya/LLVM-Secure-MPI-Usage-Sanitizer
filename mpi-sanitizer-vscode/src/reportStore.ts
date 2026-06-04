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
  summary?: MpiReportSummary;
}

export interface MpiReportSummary {
  total_events?: number;
  sends?: number;
  recvs?: number;
  collectives?: number;
  matched_pairs?: number;
  unmatched_sends?: number;
  unmatched_recvs?: number;
  errors_detected?: number;
  type_mismatch?: number;
  size_mismatch?: number;
  integrity_violation?: number;
  collective_mismatch?: number;
  deadlock_detected?: number;
  replay_detected?: number;
  timeout_warning?: number;
  anomaly_warning?: number;
  overlap_warning?: number;
  avg_p2p_latency?: number | null;
  comm_graph?: string;
}

export class ReportStore {
  private report: MpiReport = { errors: [] };
  private readonly emitter = new vscode.EventEmitter<MpiReport>();
  private watcher?: fs.FSWatcher;
  private debounce?: NodeJS.Timeout;

  constructor(private root: string, private reportPath: string) {}

  onDidUpdate = this.emitter.event;

  getReport(): MpiReport {
    return this.report;
  }

  setReportPath(root: string, reportPath: string): void {
    if (this.root === root && this.reportPath === reportPath) {
      return;
    }
    this.root = root;
    this.reportPath = reportPath;
    this.startWatching();
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
    const json: { errors?: unknown; summary?: unknown } = JSON.parse(raw);
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

    this.report = { errors: sanitized, summary: sanitizeSummary(json.summary) };
    this.emitter.fire(this.report);
  }
}

function sanitizeSummary(summary: unknown): MpiReportSummary | undefined {
  if (typeof summary !== "object" || summary === null) {
    return undefined;
  }

  const input = summary as Record<string, unknown>;
  return {
    total_events: numberValue(input.total_events),
    sends: numberValue(input.sends),
    recvs: numberValue(input.recvs),
    collectives: numberValue(input.collectives),
    matched_pairs: numberValue(input.matched_pairs),
    unmatched_sends: numberValue(input.unmatched_sends),
    unmatched_recvs: numberValue(input.unmatched_recvs),
    errors_detected: numberValue(input.errors_detected),
    type_mismatch: numberValue(input.type_mismatch),
    size_mismatch: numberValue(input.size_mismatch),
    integrity_violation: numberValue(input.integrity_violation),
    collective_mismatch: numberValue(input.collective_mismatch),
    deadlock_detected: numberValue(input.deadlock_detected),
    replay_detected: numberValue(input.replay_detected),
    timeout_warning: numberValue(input.timeout_warning),
    anomaly_warning: numberValue(input.anomaly_warning),
    overlap_warning: numberValue(input.overlap_warning),
    avg_p2p_latency: input.avg_p2p_latency === null ? null : numberValue(input.avg_p2p_latency),
    comm_graph: input.comm_graph !== undefined ? String(input.comm_graph) : undefined,
  };
}

function numberValue(value: unknown): number | undefined {
  const parsed = Number(value);
  return Number.isFinite(parsed) ? parsed : undefined;
}
