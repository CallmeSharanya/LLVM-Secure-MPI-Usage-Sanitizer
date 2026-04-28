Introduction

Message Passing Interface (MPI) is widely used for parallel programming in distributed memory systems such as clusters and high-performance computing (HPC) environments. MPI programs involve communication between multiple processes running across different nodes.

Debugging MPI programs is challenging due to:

Concurrency and non-determinism
Complex communication patterns
Lack of visibility into runtime message passing

This project proposes a compiler-integrated MPI sanitizer that instruments MPI programs at the LLVM Intermediate Representation (IR) level and detects correctness errors, communication anomalies, and security issues at runtime.

Problem Statement

Existing tools primarily focus on runtime detection of MPI errors but lack deep compiler-level integration. They also do not address network-level anomalies or communication security.

The goal of this project is to design and implement a system that:

Intercepts MPI calls at compile time using LLVM
Tracks communication patterns at runtime
Detects correctness violations such as type mismatches and deadlocks
Extends analysis to include network profiling and security validation
Objectives
Develop an LLVM pass to instrument MPI calls
Build a runtime system to track and validate communication
Detect common MPI usage errors
Provide detailed debugging information (rank, call stack, location)
Extend the system with network-aware and security-aware features
Key Features
Core Features (Assignment Requirements)
Type mismatch detection between send and receive
Buffer size mismatch detection
Buffer aliasing and overlap detection
Collective operation mismatch detection
Deadlock detection using communication graphs
Extended Features (Proposed Enhancements)
Communication latency profiling
Message size anomaly detection
Communication topology graph generation
Message integrity verification using hashing
Duplicate/replay message detection
Timeout-based failure detection
System Architecture

LLVM Pass → Instrumented Program → Runtime Library → Error Detection

LLVM Instrumentation Layer
Identifies MPI calls in LLVM IR
Extracts parameters such as buffer, datatype, count, rank
Inserts instrumentation hooks before/after MPI calls
Runtime Analysis Layer
Collects communication metadata from all processes
Matches send and receive operations
Maintains global communication state
Detects violations and reports errors
Requirements
Software Requirements
LLVM (version 14 or above)
Clang compiler
MPI implementation (OpenMPI or MPICH)
C/C++ development environment
Hardware Requirements
Multi-core system or cluster environment
Support for running MPI programs (multiple processes)
Implementation Plan
Phase 1: Setup and Basics
Install LLVM and MPI
Write and test basic MPI programs
Understand LLVM IR structure
Phase 2: LLVM Pass Development
Create a ModulePass
Identify MPI calls in IR
Extract call arguments
Insert instrumentation hooks
Phase 3: Runtime System
Implement logging functions for send/receive
Store metadata in structured format
Match communication events across ranks
Phase 4: Error Detection
Detect type mismatches
Detect buffer size inconsistencies
Detect collective mismatches
Build wait-for graph for deadlock detection
Phase 5: Testing
Develop test suite with at least 15 programs
Include both correct and buggy programs
Validate detection accuracy
Phase 6: Extensions
Add latency measurement using timestamps
Build communication graphs
Implement message hashing for integrity
Detect anomalies in message size and frequency
Add timeout-based failure detection
Evaluation
Number of detected errors
Runtime overhead introduced
Scalability with increasing number of processes
Comparison with existing tools (conceptual)
Expected Outcomes
A working LLVM-based MPI sanitizer
Detection of major MPI correctness issues
Extended capabilities for network and security analysis
Improved debugging support for distributed applications
Conclusion

This project integrates compiler techniques with distributed system analysis to provide a comprehensive debugging and validation framework for MPI programs. By extending traditional correctness checking with network and security features, it aims to offer a more robust and insightful tool for modern parallel computing environments.