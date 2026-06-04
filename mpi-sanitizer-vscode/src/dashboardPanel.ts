import * as vscode from "vscode";
import * as fs from "fs";
import * as path from "path";
import { MpiReport } from "./reportStore";

export class DashboardPanel {
  private panel: vscode.WebviewPanel | undefined;

  constructor(private readonly context: vscode.ExtensionContext) {}

  show(report: MpiReport, onNavigate: (file: string, line: number) => void): void {
    if (this.panel) {
      this.panel.reveal(vscode.ViewColumn.Beside);
      this.panel.webview.postMessage({ type: "report", report });
      return;
    }

    this.panel = vscode.window.createWebviewPanel(
      "mpiSanitize.dashboard",
      "MPI Sanitizer",
      vscode.ViewColumn.Beside,
      {
        enableScripts: true,
        retainContextWhenHidden: true,
      }
    );

    this.panel.onDidDispose(() => {
      this.panel = undefined;
    });

    this.panel.webview.onDidReceiveMessage((msg: { type?: string; file?: string; line?: number }) => {
      if (msg?.type === "navigateTo" && msg.file && msg.line) {
        onNavigate(String(msg.file), Number(msg.line));
      }
    });

    this.panel.webview.html = this.getHtml(this.panel.webview);
    this.panel.webview.postMessage({ type: "report", report });
  }

  private getHtml(webview: vscode.Webview): string {
    const htmlPath = path.join(this.context.extensionPath, "webview", "dashboard", "index.html");
    const html = fs.readFileSync(htmlPath, "utf8");
    const nonce = this.getNonce();

    const csp = `default-src 'none'; img-src ${webview.cspSource} https:; ` +
      `style-src ${webview.cspSource} 'unsafe-inline'; ` +
      `script-src 'nonce-${nonce}' https://unpkg.com;`;

    return html
      .replace("{{CSP}}", csp)
      .replace(/{{NONCE}}/g, nonce);
  }

  private getNonce(): string {
    let text = "";
    const possible = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    for (let i = 0; i < 32; i++) {
      text += possible.charAt(Math.floor(Math.random() * possible.length));
    }
    return text;
  }
}
