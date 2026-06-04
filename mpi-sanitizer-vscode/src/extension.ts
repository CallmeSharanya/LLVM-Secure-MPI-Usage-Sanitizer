import * as vscode from "vscode";
import * as path from "path";
import { LanguageClient, LanguageClientOptions, ServerOptions, TransportKind } from "vscode-languageclient/node";
import { ReportStore } from "./reportStore";
import { MpiCodeLensProvider } from "./codelens";
import { DashboardPanel } from "./dashboardPanel";

let client: LanguageClient | undefined;
let currentFilePath: string | undefined;

export function activate(context: vscode.ExtensionContext) {
  const workspaceRoot = vscode.workspace.workspaceFolders?.[0]?.uri.fsPath;
  if (!workspaceRoot) {
    return;
  }

  const config = vscode.workspace.getConfiguration("mpiSanitize");
  const reportPath = resolveWithWorkspace(config.get<string>("reportPath") || "", workspaceRoot);

  const store = new ReportStore(workspaceRoot, reportPath);
  store.startWatching();

  const dashboard = new DashboardPanel(context);

  const codelensProvider = new MpiCodeLensProvider(store, workspaceRoot);
  context.subscriptions.push(
    vscode.languages.registerCodeLensProvider([{ language: "c" }, { language: "cpp" }], codelensProvider)
  );

  currentFilePath = vscode.window.activeTextEditor?.document.uri.fsPath;
  context.subscriptions.push(
    vscode.window.onDidChangeActiveTextEditor((editor) => {
      currentFilePath = editor?.document.uri.fsPath;
    })
  );

  context.subscriptions.push(
    vscode.tasks.registerTaskProvider("mpi-sanitize", {
      provideTasks: () => {
        if (!currentFilePath) {
          return [];
        }
        return [createTaskForFile(currentFilePath, workspaceRoot)];
      },
      resolveTask: (_task) => undefined,
    })
  );

  context.subscriptions.push(
    vscode.commands.registerCommand("mpiSanitize.openDashboard", () => {
      dashboard.show(store.getReport(), (file, line) => navigateTo(file, line));
    })
  );

  context.subscriptions.push(
    vscode.commands.registerCommand("mpiSanitize.navigateTo", (args: { file: string; line: number }) => {
      if (!args) {
        return;
      }
      navigateTo(args.file, args.line);
    })
  );

  context.subscriptions.push(
    vscode.commands.registerCommand("mpiSanitize.buildAnalyze", async () => {
      const tasks = await vscode.tasks.fetchTasks({ type: "mpi-sanitize" });
      const task = tasks[0];
      if (!task) {
        vscode.window.showWarningMessage("Open a C/C++ file to run MPI Sanitize.");
        return;
      }
      vscode.tasks.executeTask(task);
    })
  );

  startLanguageServer(context, reportPath);

  store.onDidUpdate((report) => {
    dashboard.show(report, (file, line) => navigateTo(file, line));
  });
}

export function deactivate() {
  if (client) {
    return client.stop();
  }
}

function createTaskForFile(inputFile: string, root: string): vscode.Task {
  const config = vscode.workspace.getConfiguration("mpiSanitize");
  const compiler = config.get<string>("compilerPath") || "clang";
  const plugin = resolveWithWorkspace(config.get<string>("passPluginPath") || "", root);
  const runtimeLib = resolveWithWorkspace(config.get<string>("runtimeLibPath") || "", root);
  const mpirun = config.get<string>("mpiRun") || "mpirun";
  const mpiArgs = config.get<string[]>("mpiArgs") || ["-n", "4"];

  const base = path.basename(inputFile, path.extname(inputFile));
  const output = path.join(root, `${base}_san`);
  const runtimeDir = path.dirname(runtimeLib);

  const compileCmd = `${compiler} -g -O1 -fpass-plugin=${quote(plugin)} ${quote(inputFile)} ${quote(runtimeLib)} -Wl,-rpath,${quote(runtimeDir)} -o ${quote(output)}`;
  const runCmd = `${mpirun} ${mpiArgs.map(quote).join(" ")} ${quote(output)}`;
  const command = `${compileCmd} && ${runCmd}`;

  return new vscode.Task(
    { type: "mpi-sanitize" },
    vscode.TaskScope.Workspace,
    "MPI Sanitize: Build & Analyze",
    "mpi-sanitizer",
    new vscode.ShellExecution(command, { cwd: root })
  );
}

function startLanguageServer(context: vscode.ExtensionContext, reportPath: string): void {
  const serverModule = context.asAbsolutePath(path.join("out", "server.js"));
  const serverOptions: ServerOptions = {
    run: { module: serverModule, transport: TransportKind.ipc },
    debug: { module: serverModule, transport: TransportKind.ipc },
  };

  const clientOptions: LanguageClientOptions = {
    documentSelector: [{ language: "c" }, { language: "cpp" }],
    initializationOptions: {
      reportPath,
    },
  };

  client = new LanguageClient("mpiSanitizeLS", "MPI Sanitizer LSP", serverOptions, clientOptions);
  context.subscriptions.push(client.start());
}

function navigateTo(file: string, line: number): void {
  const uri = vscode.Uri.file(file);
  vscode.workspace.openTextDocument(uri).then((doc) => {
    vscode.window.showTextDocument(doc, { preview: false }).then((editor) => {
      const pos = new vscode.Position(Math.max(0, line - 1), 0);
      editor.selection = new vscode.Selection(pos, pos);
      editor.revealRange(new vscode.Range(pos, pos), vscode.TextEditorRevealType.InCenter);
    });
  });
}

function resolveWithWorkspace(value: string, root: string): string {
  if (!value) {
    return value;
  }
  const resolved = value.replace("${workspaceFolder}", root);
  return path.isAbsolute(resolved) ? resolved : path.resolve(root, resolved);
}

function quote(value: string): string {
  if (!value) {
    return value;
  }
  if (value.includes(" ") || value.includes("\t")) {
    return `"${value}"`;
  }
  return value;
}
