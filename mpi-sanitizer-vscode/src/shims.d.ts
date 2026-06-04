declare module "vscode" {
  export type Disposable = { dispose(): void };

  export class EventEmitter<T = void> {
    event: (listener: (e: T) => any) => Disposable;
    fire(data: T): void;
  }

  export interface Uri {
    fsPath: string;
  }

  export const Uri: {
    file(path: string): Uri;
  };

  export class Position {
    constructor(line: number, character: number);
  }

  export class Range {
    constructor(startLine: number, startCharacter: number, endLine: number, endCharacter: number);
    constructor(start: Position, end: Position);
  }

  export class Selection {
    constructor(start: Position, end: Position);
  }

  export enum TextEditorRevealType {
    InCenter = 0,
  }

  export enum ViewColumn {
    Beside = 2,
  }

  export interface TextDocument {
    uri: Uri;
    getText(): string;
  }

  export interface TextEditor {
    selection: Selection;
    revealRange(range: Range, revealType: TextEditorRevealType): void;
  }

  export interface Webview {
    cspSource: string;
    html: string;
    postMessage(message: unknown): Thenable<boolean>;
    onDidReceiveMessage(listener: (message: unknown) => void): Disposable;
  }

  export interface WebviewPanel {
    webview: Webview;
    reveal(viewColumn: ViewColumn): void;
    onDidDispose(listener: () => void): Disposable;
  }

  export interface ExtensionContext {
    extensionPath: string;
    subscriptions: Disposable[];
    asAbsolutePath(relativePath: string): string;
  }

  export interface WorkspaceFolder {
    uri: Uri;
  }

  export interface WorkspaceConfiguration {
    get<T>(section: string): T | undefined;
  }

  export interface TaskExecution {}

  export interface TaskDefinition {
    type: string;
  }

  export class ShellExecution {
    constructor(commandLine: string, options?: { cwd?: string });
  }

  export enum TaskScope {
    Workspace = 1,
  }

  export class Task {
    constructor(definition: TaskDefinition, scope: TaskScope, name: string, source: string, execution: ShellExecution);
  }

  export interface TaskProvider {
    provideTasks(): Task[] | Thenable<Task[]>;
    resolveTask?(task: Task): Task | undefined | Thenable<Task | undefined>;
  }

  export interface LanguageSelector {
    language: string;
  }

  export interface CodeLensProvider {
    provideCodeLenses(document: TextDocument): CodeLens[];
  }

  export class CodeLens {
    constructor(range: Range, command?: { title: string; command: string; arguments?: unknown[] });
  }

  export interface Command {
    title: string;
    command: string;
    arguments?: unknown[];
  }

  export interface Tasks {
    registerTaskProvider(type: string, provider: TaskProvider): Disposable;
    fetchTasks(filter: { type: string }): Thenable<Task[]>;
    executeTask(task: Task): Thenable<TaskExecution>;
  }

  export interface Window {
    activeTextEditor: TextEditor | undefined;
    onDidChangeActiveTextEditor(listener: (editor: TextEditor | undefined) => void): Disposable;
    showWarningMessage(message: string): void;
    showTextDocument(document: TextDocument, options?: { preview?: boolean }): Thenable<TextEditor>;
    createWebviewPanel(viewType: string, title: string, showOptions: ViewColumn, options: { enableScripts: boolean; retainContextWhenHidden: boolean }): WebviewPanel;
  }

  export interface Workspace {
    workspaceFolders: WorkspaceFolder[] | undefined;
    getConfiguration(section: string): WorkspaceConfiguration;
    openTextDocument(uri: Uri): Thenable<TextDocument>;
    registerCodeLensProvider(selector: LanguageSelector[], provider: CodeLensProvider): Disposable;
  }

  export const window: Window;
  export const workspace: Workspace;
  export const tasks: Tasks;
  export const commands: {
    registerCommand(command: string, callback: (...args: any[]) => any): Disposable;
  };

  export interface PositionLike {
    line: number;
    character: number;
  }

  export const languages: {
    registerCodeLensProvider(selector: LanguageSelector[], provider: CodeLensProvider): Disposable;
  };
}

declare module "vscode-languageclient/node" {
  export const TransportKind: { ipc: number };
  export class LanguageClient {
    constructor(id: string, name: string, serverOptions: unknown, clientOptions: unknown);
    start(): unknown;
    stop(): unknown;
  }
}