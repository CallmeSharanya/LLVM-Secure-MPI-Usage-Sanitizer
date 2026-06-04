import * as vscode from "vscode";
import * as path from "path";
import { ReportStore } from "./reportStore";

const MPI_CALL_RE = /\bMPI_(Send|Recv|Bcast|Reduce|Allreduce|Barrier)\b/;

export class MpiCodeLensProvider implements vscode.CodeLensProvider {
  private readonly emitter = new vscode.EventEmitter<void>();
  onDidChangeCodeLenses = this.emitter.event;

  constructor(private readonly store: ReportStore, private readonly root: string) {
    this.store.onDidUpdate(() => this.emitter.fire());
  }

  provideCodeLenses(document: vscode.TextDocument): vscode.CodeLens[] {
    const lenses: vscode.CodeLens[] = [];
    const lines = document.getText().split(/\r?\n/);

    for (let i = 0; i < lines.length; i++) {
      if (!MPI_CALL_RE.test(lines[i])) {
        continue;
      }

      const range = new vscode.Range(i, 0, i, 0);
      const filePath = document.uri.fsPath;
      const entries = this.store.getEntriesForFileLine(filePath, i + 1);

      if (entries.length === 0) {
        lenses.push(
          new vscode.CodeLens(range, {
            title: "\u27A4 re-run sanitizer",
            command: "mpiSanitize.buildAnalyze",
          })
        );
        continue;
      }

      for (const entry of entries) {
        const labelType = entry.type ? entry.type.replace(/_/g, " ") : "issue";
        lenses.push(
          new vscode.CodeLens(range, {
            title: `\u26A0 [rank ${entry.rank ?? "?"}] ${labelType}`,
            command: "mpiSanitize.openDashboard",
          })
        );

        if (entry.peer_file && entry.peer_line) {
          const peerPath = path.isAbsolute(entry.peer_file)
            ? entry.peer_file
            : path.resolve(this.root, entry.peer_file);
          lenses.push(
            new vscode.CodeLens(range, {
              title: "\u25B6 deadlock partner",
              command: "mpiSanitize.navigateTo",
              arguments: [{ file: peerPath, line: entry.peer_line }],
            })
          );
        }
      }
    }

    return lenses;
  }
}
