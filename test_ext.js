const fs = require("fs");
const cp = require("child_process");

const params = {"compiler":"clang","mpiCompiler":"mpicc","opt":"opt","plugin":"c:\\Users\\Shreya Prasad\\Desktop\\LLVM-Secure-MPI-Usage-Sanitizer/build/libMPISanitizePass.so","runtimeLib":"c:\\Users\\Shreya Prasad\\Desktop\\LLVM-Secure-MPI-Usage-Sanitizer/build/libmsan_runtime.so","runtimeDir":"c:\\Users\\Shreya Prasad\\Desktop\\LLVM-Secure-MPI-Usage-Sanitizer/build","mpiArgs":["-n","4"],"inputFile":"c:\\Users\\Shreya Prasad\\Desktop\\LLVM-Secure-MPI-Usage-Sanitizer\\tests\\deadlock.c","bcFile":"c:\\Users\\Shreya Prasad\\Desktop\\LLVM-Secure-MPI-Usage-Sanitizer\\.mpi-sanitize\\deadlock\\deadlock.bc","instBcFile":"c:\\Users\\Shreya Prasad\\Desktop\\LLVM-Secure-MPI-Usage-Sanitizer\\.mpi-sanitize\\deadlock\\deadlock.inst.bc","objFile":"c:\\Users\\Shreya Prasad\\Desktop\\LLVM-Secure-MPI-Usage-Sanitizer\\.mpi-sanitize\\deadlock\\deadlock.o","output":"c:\\Users\\Shreya Prasad\\Desktop\\LLVM-Secure-MPI-Usage-Sanitizer\\.mpi-sanitize\\deadlock\\deadlock_san","outDir":"c:\\Users\\Shreya Prasad\\Desktop\\LLVM-Secure-MPI-Usage-Sanitizer\\.mpi-sanitize\\deadlock","root":"c:\\Users\\Shreya Prasad\\Desktop\\LLVM-Secure-MPI-Usage-Sanitizer","mpirun":"mpirun"};

const options = params;
const isWin = process.platform === "win32";

function toWslPath(p) {
  if (!isWin || typeof p !== "string" || (!p.includes(":\\") && !p.includes(":/"))) return p;
  return p.replace(/^([A-Za-z]):[\\\/]/, (m, drive) => `/mnt/${drive.toLowerCase()}/`).replace(/\\/g, "/");
}

function run(command, args) {
  const mappedArgs = args.map(toWslPath);
  if (isWin && !command.startsWith("wsl")) {
    mappedArgs.unshift(command);
    command = "wsl";
  }
  console.log("RUNNING:", command, mappedArgs.join(" "));
  cp.execFileSync(command, mappedArgs, { stdio: "inherit", cwd: options.root });
}

function splitArgs(text) {
  if (!text) return [];
  const args = [];
  const re = /(?:[^\s"']+|"(?:[^"\\]|\\.)*"|'(?:[^'\\]|\\.)*')+/g;
  for (const match of text.match(re) || []) {
    if ((match.startsWith('"') && match.endsWith('"')) || (match.startsWith("'") && match.endsWith("'"))) {
      args.push(match.slice(1, -1));
    } else {
      args.push(match);
    }
  }
  return args;
}

fs.mkdirSync(options.outDir, { recursive: true });

let mpiCmd = options.mpiCompiler;
let mpiShowmeArgs = ["--showme:compile"];
if (isWin && !mpiCmd.startsWith("wsl")) {
  mpiShowmeArgs.unshift(mpiCmd);
  mpiCmd = "wsl";
}

console.log("FETCHING compileFlags with:", mpiCmd, mpiShowmeArgs.join(" "));
const compileFlags = cp.execFileSync(mpiCmd, mpiShowmeArgs, {
  encoding: "utf8",
  cwd: options.root,
}).trim();

run(options.compiler, ["-g", "-O0", "-emit-llvm", "-c", ...splitArgs(compileFlags), options.inputFile, "-o", options.bcFile]);
run(options.opt, ["-load-pass-plugin=" + toWslPath(options.plugin), "-passes=mpi-sanitize", options.bcFile, "-o", options.instBcFile]);
run(options.compiler, ["-g", "-O0", "-c", options.instBcFile, "-o", options.objFile]);
run(options.mpiCompiler, ["-g", "-O0", options.objFile, toWslPath(options.runtimeLib), "-lm", "-Wl,-rpath," + toWslPath(options.runtimeDir), "-o", options.output]);
run(options.mpirun, [...options.mpiArgs, options.output]);
