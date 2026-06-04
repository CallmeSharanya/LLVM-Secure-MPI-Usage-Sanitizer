import * as vscode from "vscode";
import * as path from "path";
import * as fs from "fs";
import { LanguageClient, TransportKind } from "vscode-languageclient/node";
import { ReportStore } from "./reportStore";
import { MpiCodeLensProvider } from "./codelens";
import { DashboardPanel } from "./dashboardPanel";

let client: any;
let currentFilePath: string | undefined;

const C_CPP_EXTENSIONS = new Set([".c", ".cc", ".cpp", ".cxx"]);
const C_CPP_LANGUAGES = new Set(["c", "cpp"]);

export function activate(context: vscode.ExtensionContext) {
  const workspaceRoot = getWorkspaceRoot(context);

  const config = vscode.workspace.getConfiguration("mpiSanitize");
  const reportPath = resolveWithWorkspace(config.get<string>("reportPath") || "", workspaceRoot);

  const store = new ReportStore(workspaceRoot, reportPath);
  store.startWatching();

  const dashboard = new DashboardPanel(context);

  const codelensProvider = new MpiCodeLensProvider(store, workspaceRoot);
  context.subscriptions.push(
    vscode.languages.registerCodeLensProvider([{ language: "c" }, { language: "cpp" }], codelensProvider)
  );

  currentFilePath = getEditorCFile(vscode.window.activeTextEditor);
  context.subscriptions.push(
    vscode.window.onDidChangeActiveTextEditor((editor) => {
      const filePath = getEditorCFile(editor);
      if (filePath) {
        currentFilePath = filePath;
      }
    })
  );

  context.subscriptions.push(
    vscode.tasks.registerTaskProvider("mpi-sanitize", {
      provideTasks: () => {
        if (!currentFilePath) {
          return [];
        }
        return [createTaskForFile(currentFilePath, getWorkspaceRootForFile(currentFilePath))];
      },
      resolveTask: (_task: vscode.Task) => undefined,
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
      await runBuildAnalyze(store);
    })
  );

  context.subscriptions.push(
    vscode.commands.registerCommand("mpiSanitize.buildAnalyse", async () => {
      await runBuildAnalyze(store);
    })
  );

  startLanguageServer(context, reportPath);

  store.onDidUpdate((report: import("./reportStore").MpiReport) => {
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
  const isCpp = isCppFile(inputFile);
  const compiler =
    config.get<string>(isCpp ? "cxxCompilerPath" : "compilerPath") || (isCpp ? "clang++" : "clang");
  const mpiCompiler =
    config.get<string>(isCpp ? "mpiCxxCompilerPath" : "mpiCompilerPath") || (isCpp ? "mpicxx" : "mpicc");
  const opt = config.get<string>("optPath") || "opt";
  const plugin = resolveBuildArtifact(config.get<string>("passPluginPath") || "", root, "libMPISanitizePass.so");
  const runtimeLib = resolveBuildArtifact(config.get<string>("runtimeLibPath") || "", root, "libmsan_runtime.so");
  const mpirun = config.get<string>("mpiRun") || "mpirun";
  const mpiArgs = config.get<string[]>("mpiArgs") || ["-n", "4"];

  const base = path.basename(inputFile, path.extname(inputFile));
  const outDir = path.join(root, ".mpi-sanitize", base);
  const bcFile = path.join(outDir, `${base}.bc`);
  const instBcFile = path.join(outDir, `${base}.inst.bc`);
  const objFile = path.join(outDir, `${base}.o`);
  const output = path.join(outDir, `${base}_san`);
  const runtimeDir = path.dirname(runtimeLib);

  const mpiCompileFlags = `$(${quote(mpiCompiler)} --showme:compile)`;
  const makeOutDirCmd = `mkdir -p ${quote(outDir)}`;
  const emitBcCmd = `${quote(compiler)} -g -O0 -emit-llvm -c ${mpiCompileFlags} ${quote(inputFile)} -o ${quote(bcFile)}`;
  const optCmd = `${quote(opt)} -load-pass-plugin=${quote(plugin)} -passes=mpi-sanitize ${quote(bcFile)} -o ${quote(instBcFile)}`;
  const objCmd = `${quote(compiler)} -g -O0 -c ${quote(instBcFile)} -o ${quote(objFile)}`;
  const linkCmd = `${quote(mpiCompiler)} -g -O0 ${quote(objFile)} ${quote(runtimeLib)} -lm -Wl,-rpath,${quote(runtimeDir)} -o ${quote(output)}`;
  const runCmd = `${mpirun} ${mpiArgs.map(quote).join(" ")} ${quote(output)}`;
  const hasWorkspace = Boolean(vscode.workspace.workspaceFolders?.length);
  const baseCommand = `${makeOutDirCmd} && ${emitBcCmd} && ${optCmd} && ${objCmd} && ${linkCmd} && ${runCmd}`;
  const command = hasWorkspace ? baseCommand : `cd ${quote(root)} && ${baseCommand}`;
  const taskScope = hasWorkspace ? vscode.TaskScope.Workspace : vscode.TaskScope.Global;
  const shellOptions = hasWorkspace ? { cwd: root } : undefined;

  return new vscode.Task(
    { type: "mpi-sanitize" },
    taskScope,
    "MPI Sanitize: Build & Analyze",
    "mpi-sanitizer",
    new vscode.ShellExecution(command, shellOptions)
  );
}

async function runBuildAnalyze(store: ReportStore): Promise<void> {
  const inputFile = await selectCFile();
  if (!inputFile) {
    vscode.window.showWarningMessage("Open a C/C++ file to run MPI Sanitize.");
    return;
  }
  currentFilePath = inputFile;

  const root = getWorkspaceRootForFile(inputFile);
  const config = vscode.workspace.getConfiguration("mpiSanitize");
  const reportPath = resolveWithWorkspace(config.get<string>("reportPath") || "", root);
  store.setReportPath(root, reportPath);

  const task = createTaskForFile(inputFile, root);
  await vscode.tasks.executeTask(task);
}

function startLanguageServer(context: vscode.ExtensionContext, reportPath: string): void {
  const serverModule = context.asAbsolutePath(path.join("out", "server.js"));
  const serverOptions = {
    run: { module: serverModule, transport: TransportKind.ipc },
    debug: { module: serverModule, transport: TransportKind.ipc },
  };

  const clientOptions = {
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
  vscode.workspace.openTextDocument(uri).then((doc: vscode.TextDocument) => {
    vscode.window.showTextDocument(doc, { preview: false }).then((editor: vscode.TextEditor) => {
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

function resolveBuildArtifact(value: string, root: string, fileName: string): string {
  const resolved = resolveWithWorkspace(value, root);
  if (resolved && fs.existsSync(resolved)) {
    return resolved;
  }

  const repoRoot = findProjectRoot(root) || root;
  const fallback = path.join(repoRoot, "build", fileName);
  return fs.existsSync(fallback) ? fallback : resolved;
}

function getWorkspaceRoot(context: vscode.ExtensionContext): string {
  const folder = vscode.workspace.workspaceFolders?.[0]?.uri.fsPath;
  if (folder) {
    return folder;
  }
  return context.extensionPath;
}

function getWorkspaceRootForFile(filePath: string): string {
  const projectRoot = findProjectRoot(filePath);
  if (projectRoot) {
    return projectRoot;
  }

  const folders = vscode.workspace.workspaceFolders || [];
  const containing = folders.find((folder) => {
    const root = folder.uri.fsPath;
    const rel = path.relative(root, filePath);
    return rel && !rel.startsWith("..") && !path.isAbsolute(rel);
  });
  return containing?.uri.fsPath || folders[0]?.uri.fsPath || path.dirname(filePath);
}

function findProjectRoot(startPath: string): string | undefined {
  let dir = fs.existsSync(startPath) && fs.statSync(startPath).isDirectory() ? startPath : path.dirname(startPath);

  while (true) {
    if (hasSanitizerBuild(dir) || hasProjectLayout(dir)) {
      return dir;
    }

    if (path.basename(dir) === "mpi-sanitizer-vscode") {
      const parent = path.dirname(dir);
      if (hasSanitizerBuild(parent) || hasProjectLayout(parent)) {
        return parent;
      }
    }

    const parent = path.dirname(dir);
    if (parent === dir) {
      return undefined;
    }
    dir = parent;
  }
}

function hasSanitizerBuild(dir: string): boolean {
  return (
    fs.existsSync(path.join(dir, "build", "libMPISanitizePass.so")) &&
    fs.existsSync(path.join(dir, "build", "libmsan_runtime.so"))
  );
}

function hasProjectLayout(dir: string): boolean {
  return (
    fs.existsSync(path.join(dir, "CMakeLists.txt")) &&
    fs.existsSync(path.join(dir, "passes", "MPISanitizePass.cpp")) &&
    fs.existsSync(path.join(dir, "runtime", "runtime.c"))
  );
}

async function selectCFile(): Promise<string | undefined> {
  const activeFile = getEditorCFile(vscode.window.activeTextEditor);
  if (activeFile) {
    return activeFile;
  }

  if (currentFilePath && isCOrCppFile(currentFilePath)) {
    return currentFilePath;
  }

  for (const editor of vscode.window.visibleTextEditors || []) {
    const filePath = getEditorCFile(editor);
    if (filePath) {
      return filePath;
    }
  }

  const files = await vscode.workspace.findFiles("**/*.{c,cc,cpp,cxx}", "**/{.git,build,node_modules,out}/**", 100);
  if (files.length === 1) {
    return files[0].fsPath;
  }
  if (files.length > 1) {
    const picked = await vscode.window.showQuickPick(
      files.map((uri) => ({
        label: path.basename(uri.fsPath),
        description: vscode.workspace.asRelativePath(uri.fsPath),
        filePath: uri.fsPath,
      })),
      { placeHolder: "Select the MPI C/C++ file to sanitize" }
    );
    return picked?.filePath;
  }

  return undefined;
}

function getEditorCFile(editor: vscode.TextEditor | undefined): string | undefined {
  if (!editor) {
    return undefined;
  }
  const filePath = editor.document.uri.fsPath;
  if (!filePath) {
    return undefined;
  }
  return C_CPP_LANGUAGES.has(editor.document.languageId) || isCOrCppFile(filePath) ? filePath : undefined;
}

function isCOrCppFile(filePath: string): boolean {
  return C_CPP_EXTENSIONS.has(path.extname(filePath).toLowerCase());
}

function isCppFile(filePath: string): boolean {
  return [".cc", ".cpp", ".cxx"].includes(path.extname(filePath).toLowerCase());
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
