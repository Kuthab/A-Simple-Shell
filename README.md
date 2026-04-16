Kuthab Ibrahim ki91


================================================================================
mysh - A Simple Shell
================================================================================

Building
--------
    make

This produces the mysh executable.

Running
-------
Interactive mode:
    ./mysh

Batch mode (from file):
    ./mysh script.sh

Batch mode (from stdin):
    echo "echo hello" | ./mysh


Design
------
The shell is implemented in a single source file, mysh.c. The major components
are:

1. Input reading - read_line() uses read() one byte at a time to collect a full
   line before returning. This ensures we never call read() after receiving a
   newline until the command has been executed, which keeps interactive mode
   responsive.

2. Tokeniser - tokenise() splits a line into tokens. Whitespace separates
   tokens; <, >, and | are always their own token. # begins a comment that
   discards the rest of the line.

3. Wildcard expansion - During parsing, any token containing * is expanded using
   opendir/readdir. The pattern supports a single * with a prefix and suffix
   match. Hidden files (starting with .) are not matched when the pattern begins
   with *. If no files match, the token is passed through unchanged.

4. Command parser - parse_command() builds an array of subcmd structs (one per
   pipeline segment). Each subcmd has an argv array, optional input redirection
   file, and optional output redirection file. Syntax errors (e.g., < <,
   trailing |) return -1 and the shell continues to the next command.

5. Built-in commands - cd, pwd, which, and exit are handled directly by the
   shell. When used alone, they run in the parent process. When part of a
   pipeline, they run in a forked child so pipes work correctly.

6. Execution - For external commands, execv() is used (never execvp). Bare
   names are searched in /usr/local/bin, /usr/bin, /bin in order using
   access(). Pipelines are set up with pipe() and dup2().

7. Interactive mode - Detected via isatty(). Prints welcome/goodbye messages,
   a prompt showing the working directory (with ~ substitution for HOME), and
   exit status feedback for failed/signalled commands.

8. Batch mode - No prompts or messages. Child processes receive /dev/null as
   their default stdin (unless input redirection or a pipe overrides it).

9. Conditionals - "then" and "else" keywords at the start of a command line
   allow conditional execution based on the previous command's exit status.
   "then" executes its command only if the previous command succeeded (exit 0).
   "else" executes its command only if the previous command failed (non-zero
   exit). Skipped commands do not change the status, allowing chains like
   "then ... then ..." or "else ... then ...".


Test Plan
---------
Tests are automated in test.sh and can be run with "make test". We chose to
automate our tests with a shell script that compares expected vs actual output
for each scenario. This makes it easy to re-run the full suite whenever we
change the code, catching regressions immediately.

Our testing strategy covers every feature of the shell in isolation first, then
in combination. For each test, we pipe commands into mysh in batch mode and
compare stdout against expected output. Error cases are tested by redirecting
stderr to /dev/null and verifying the shell continues to the next command.

Test categories and cases:

  Basic commands (batch mode)
    - "echo hello" : verifies simple command execution and argument passing
    - "echo hello world" : verifies multiple arguments are forwarded correctly
    - Empty command : verifies the shell does nothing and does not crash
    - Comment-only line (# comment) : verifies it is treated as empty
    - "echo hello # comment" : verifies comment stripping mid-line

  Built-in: pwd
    - "pwd" : output compared against actual working directory from the test
      harness; verifies getcwd() integration

  Built-in: cd
    - "cd /tmp" then "pwd" : verifies directory actually changes
    - "cd" with no args : verifies it changes to HOME directory
    - "cd /tmp", "cd ..", "pwd" : verifies relative path navigation works
    - "cd /nonexistent_dir" : verifies error message is printed and shell
      continues (does not crash)

  Built-in: which
    - "which ls" : verifies it finds ls in /usr/bin or /bin
    - "which cd" : verifies built-in names are rejected (prints nothing, fails)
    - "which nonexistent_program" : verifies unknown programs fail gracefully

  Built-in: exit
    - "exit" followed by "echo" : verifies shell stops immediately and does not
      execute further commands

  EOF terminates (die)
    - Input ends without "exit" command : verifies shell terminates cleanly on
      EOF, printing goodbye message in interactive mode

  Launch program by absolute path
    - "/bin/echo path_test" : verifies that a command containing / is treated
      as a direct path to an executable and run with execv()

  Launch program by bare name
    - "echo name_test" : verifies the shell searches /usr/local/bin, /usr/bin,
      /bin in order and finds the correct executable

  Output redirection
    - "echo redirected > file" : verifies file is created with correct content
    - File permissions : verifies created file has mode 0640 (rw-r-----)

  Input redirection
    - "cat < file" : verifies file contents are provided as stdin to the child

  Both redirections together
    - "cat < infile > outfile" : verifies both work simultaneously
    - "cat > outfile < infile" : verifies order does not matter

  Pipes
    - "echo hello | cat" : verifies a simple single pipe
    - "echo hello world | wc -w" : verifies data flows correctly through pipe
    - "echo abc | cat | cat | cat" : verifies multi-stage pipeline (3 pipes)

  Wildcards
    - "echo dir/*.txt" : verifies matching files are expanded and sorted
    - "echo dir/*.xyz" : verifies no-match passes the token through unchanged
    - Hidden files : verifies * does not match files starting with .

  Conditionals (then/else)
    - "echo ok" then "then echo yes" : then runs after successful command
    - "ls /nonexistent" then "then echo x" : then is skipped after failure
    - "ls /nonexistent" then "else echo fallback" : else runs after failure
    - "echo ok" then "else echo x" : else is skipped after success
    - Chained "then ... then ..." : consecutive conditionals propagate status
    - "else ... then ..." chain : recovery after failure, then success chain

  Batch mode stdin
    - "cat" with no arguments in piped batch mode : verifies child process
      receives /dev/null as stdin (produces no output, does not hang)

  Syntax errors
    - "< <" followed by "echo after_error" : verifies the shell detects the
      syntax error, skips the bad command, and continues to the next line
    - "|" on its own followed by "echo" : verifies leading pipe is an error
      but recovery works

  Batch mode from file
    - Single command script : verifies ./mysh script.sh mode works
    - Multi-line script : verifies sequential execution of all lines

  Batch and interactive mode
    - Interactive mode is tested manually by running ./mysh in a terminal and
      verifying: welcome message appears, prompt shows ~$ or full path,
      exit status messages print for failed commands, goodbye message prints
      on exit or EOF. Batch mode is tested by all the automated tests above
      (piped input is non-tty, so batch mode is used automatically).

  Pipeline with exit
    - "echo text | cat" : verifies pipes execute correctly; "foo | exit"
      terminates mysh after the pipeline completes
