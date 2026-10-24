#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>
#include <glob.h>
#include <wordexp.h>
#include <vector>
#include <string>
#include <cctype>
#include <iostream>
#include <sstream>
#include "command.h"
#include "tokenizer.h"

int logFd = -1; // shell.log
// write a line in shell.log when a child finishes
void logChild(pid_t pid, int status)
{
    if (logFd < 0)
        return;

    char msg[100];
    int len;
    if (WIFSIGNALED(status))
        len = snprintf(msg, sizeof(msg), "Child %d killed by signal %d\n", (int)pid, WTERMSIG(status));
    else
        len = snprintf(msg, sizeof(msg), "Child %d terminated, exit status %d\n", (int)pid, WEXITSTATUS(status));

    ssize_t w = write(logFd, msg, len);
    (void)w;
}
// SIGCHLD handler: collect every child that finished and log it
void childHandler(int sig)
{
    int oldErrno = errno;
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
        logChild(pid, status);

    errno = oldErrno;
}
// the tokenizer cuts on spaces, so "Hello, World!" comes as 2 tokens.
// this puts them back together and removes the quotes.
// quoted[i] is true if token i came from inside quotes (no wildcards for those)
bool joinQuotes(std::vector<Token> &in, std::vector<Token> &out, std::vector<bool> &quoted)
{
    for (size_t i = 0; i < in.size(); i++)
    {
        std::string word = in[i].value;

        if (in[i].type != TOKEN_EOF && !word.empty() && (word[0] == '"' || word[0] == '\''))
        {
            char q = word[0];
            size_t j = i;

            // keep adding tokens until we find the closing quote
            while (word.find(q, 1) == std::string::npos)
            {
                if (j + 1 >= in.size() || in[j + 1].type == TOKEN_EOF)
                    return false;
                j++;
                word += " " + in[j].value;
            }

            size_t end = word.find(q, 1);
            word = word.substr(1, end - 1) + word.substr(end + 1);

            out.push_back({TOKEN_ARGUMENT, word});
            quoted.push_back(true);
            i = j;
        }
        else
        {
            out.push_back(in[i]);
            quoted.push_back(false);
        }
    }
    return true;
}
// add a word to the command, if it has * or ? replace it with the files that match
void addArgument(SimpleCommand *sc, const std::string &word, bool quoted)
{
    if (!quoted && word.find_first_of("*?") != std::string::npos)
    {
        glob_t g;
        // GLOB_NOCHECK: if nothing matches we keep the word like it was
        if (glob(word.c_str(), GLOB_NOCHECK, NULL, &g) == 0)
        {
            for (size_t i = 0; i < g.gl_pathc; i++)
                sc->insertArgument(strdup(g.gl_pathv[i]));
            globfree(&g);
            return;
        }
    }
    sc->insertArgument(strdup(word.c_str()));
}
// print the error and throw away what we built so far
void syntaxError(const char *msg, SimpleCommand *sc)
{
    fprintf(stderr, "myshell: syntax error: %s\n", msg);

    for (int i = 0; i < sc->_numberOfArguments; i++)
        free(sc->_arguments[i]);
    free(sc->_arguments);
    delete sc;

    Command::_currentCommand.clear();
}
// replace the old file name law fy  with a new one
void setFile(char *&dest, const char *name)
{
    if (dest)
        free(dest);
    dest = strdup(name);
}

void parse(std::vector<Token> &rawTokens)
{
    std::vector<Token> tokens;
    std::vector<bool> quoted;

    if (!joinQuotes(rawTokens, tokens, quoted))
    {
        fprintf(stderr, "myshell: syntax error: unterminated quote\n");
        return;
    }

    Command &cmd = Command::_currentCommand;
    SimpleCommand *sc = new SimpleCommand();
    bool afterPipe = false;

    for (size_t i = 0; i < tokens.size(); i++)
    {
        Token &t = tokens[i];

        switch (t.type)
        {
        case TOKEN_COMMAND:
        case TOKEN_ARGUMENT:
            addArgument(sc, t.value, quoted[i]);
            afterPipe = false;
            break;

        case TOKEN_REDIRECT:
        case TOKEN_APPEND:
        case TOKEN_INPUT:
        case TOKEN_ERROR:
        case TOKEN_REDIRECT_AND_ERROR:
        {
            // the next token has to be the file name
            if (i + 1 >= tokens.size() ||
                (tokens[i + 1].type != TOKEN_COMMAND && tokens[i + 1].type != TOKEN_ARGUMENT))
                return syntaxError("missing file name after redirection", sc);

            const char *file = tokens[i + 1].value.c_str();
            i++;

            if (t.type == TOKEN_INPUT)
            {
                setFile(cmd._inputFile, file);
            }
            else if (t.type == TOKEN_ERROR)
            {
                setFile(cmd._errFile, file);
            }
            else if (t.type == TOKEN_REDIRECT)
            {
                setFile(cmd._outFile, file);
                cmd._append = 0;
            }
            else if (t.type == TOKEN_APPEND)
            {
                setFile(cmd._outFile, file);
                cmd._append = 1;
            }
            else // >>& : stdout and stderr to the same file,( append)
            {
                setFile(cmd._outFile, file);
                setFile(cmd._errFile, file);
                cmd._append = 1;
                cmd._out_error = 1;
            }
            afterPipe = false;
            break;
        }

        case TOKEN_PIPE:
            if (sc->_numberOfArguments == 0)
                return syntaxError("missing command before '|'", sc);
            cmd.insertSimpleCommand(sc);
            sc = new SimpleCommand();
            afterPipe = true;
            break;

        case TOKEN_BACKGROUND:
            // & is only allowed at the very end
            if (sc->_numberOfArguments == 0 || tokens[i + 1].type != TOKEN_EOF)
                return syntaxError("'&' must come at the end of a command", sc);
            cmd._background = 1;
            break;

        case TOKEN_EOF:
            if (afterPipe)
                return syntaxError("missing command after '|'", sc);

            if (sc->_numberOfArguments == 0)
            {
                bool emptyLine = cmd._numberOfSimpleCommands == 0 &&
                                 !cmd._outFile && !cmd._inputFile &&
                                 !cmd._errFile && !cmd._background;
                if (emptyLine)
                {
                    // user just pressed enter
                    free(sc->_arguments);
                    delete sc;
                    return;
                }
                return syntaxError("missing command", sc);
            }

            cmd.insertSimpleCommand(sc);
            cmd.execute();
            return;
        }
    }
}

SimpleCommand::SimpleCommand()
{
    _numberOfAvailableArguments = 5;
    _numberOfArguments = 0;
    _arguments = (char **)malloc(_numberOfAvailableArguments * sizeof(char *));
}
void SimpleCommand::insertArgument(char *argument)
{
    if (_numberOfAvailableArguments == _numberOfArguments + 1)
    {
        _numberOfAvailableArguments *= 2;
        _arguments = (char **)realloc(_arguments,
                                      _numberOfAvailableArguments * sizeof(char *));
    }

    _arguments[_numberOfArguments] = argument;
    _arguments[_numberOfArguments + 1] = NULL;

    _numberOfArguments++;
}

Command::Command()
{
    _numberOfAvailableSimpleCommands = 1;
    _simpleCommands = (SimpleCommand **)
        malloc(_numberOfAvailableSimpleCommands * sizeof(SimpleCommand *));

    _numberOfSimpleCommands = 0;
    _outFile = 0;
    _inputFile = 0;
    _errFile = 0;
    _out_error = 0;
    _append = 0;
    _background = 0;
}
void Command::insertSimpleCommand(SimpleCommand *simpleCommand)
{
    if (_numberOfAvailableSimpleCommands == _numberOfSimpleCommands)
    {
        _numberOfAvailableSimpleCommands *= 2;
        _simpleCommands = (SimpleCommand **)realloc(_simpleCommands,
                                                    _numberOfAvailableSimpleCommands * sizeof(SimpleCommand *));
    }

    _simpleCommands[_numberOfSimpleCommands] = simpleCommand;
    _numberOfSimpleCommands++;
}
void Command::clear()
{
    for (int i = 0; i < _numberOfSimpleCommands; i++)
    {
        for (int j = 0; j < _simpleCommands[i]->_numberOfArguments; j++)
        {
            free(_simpleCommands[i]->_arguments[j]);
        }

        free(_simpleCommands[i]->_arguments);
        delete _simpleCommands[i];
    }

    if (_outFile)
    {
        free(_outFile);
    }

    if (_inputFile)
    {
        free(_inputFile);
    }

    if (_errFile)
    {
        free(_errFile);
    }

    _numberOfSimpleCommands = 0;
    _outFile = 0;
    _inputFile = 0;
    _errFile = 0;
    _out_error = 0;
    _append = 0;
    _background = 0;
}
void Command::print()
{
    printf("\n\n");
    printf("              COMMAND TABLE                \n");
    printf("\n");
    printf("  #   Simple Commands\n");
    printf("  --- ----------------------------------------------------------\n");

    for (int i = 0; i < _numberOfSimpleCommands; i++)
    {
        printf("  %-3d ", i);
        for (int j = 0; j < _simpleCommands[i]->_numberOfArguments; j++)
        {
            printf("\"%s\" \t", _simpleCommands[i]->_arguments[j]);
        }
        printf("\n");
    }

    printf("\n\n");
    printf("  Output       Input        Error        Err&Out       Background\n");
    printf("  ------------ ------------ ------------ ------------ ------------\n");
    printf("  %-12s %-12s %-12s %-12s %-12s\n", _outFile ? _outFile : "default",
           _inputFile ? _inputFile : "default", _errFile ? _errFile : "default",
           _out_error == 1 ? _errFile : "default",
           _background ? "YES" : "NO");
    printf("\n\n");
}
void Command::execute()
{
    if (_numberOfSimpleCommands == 0)
    {
        clear();
        return;
    }
    // print();   // uncomment to see the command table

    // exit and cd lazem ye run in shell, not in a child
    if (_numberOfSimpleCommands == 1)
    {
        SimpleCommand *first = _simpleCommands[0];

        if (strcmp(first->_arguments[0], "exit") == 0)
        {
            printf(" Good bye!!\n");
            clear();
            exit(0);
        }

        if (strcmp(first->_arguments[0], "cd") == 0)
        {
            if (first->_numberOfArguments > 2)
            {
                fprintf(stderr, "myshell: cd: too many arguments\n");
            }
            else
            {
                // no argument means go home
                const char *dir = getenv("HOME");
                if (first->_numberOfArguments == 2)
                    dir = first->_arguments[1];

                if (chdir(dir) != 0)
                    fprintf(stderr, "myshell: cd: %s: %s\n", dir, strerror(errno));
            }
            clear();
            return;
        }
    }
    // keep copies of stdin/stdout/stderr to put them back at the end
    int defIn = dup(0);
    int defOut = dup(1);
    int defErr = dup(2);

    int fdIn = -1;
    int fdOut = -1;
    bool ok = true;
    // open the redirection files
    if (_inputFile)
    {
        fdIn = open(_inputFile, O_RDONLY);
        if (fdIn < 0)
        {
            fprintf(stderr, "myshell: %s: %s\n", _inputFile, strerror(errno));
            ok = false;
        }
    }

    if (ok && _outFile)
    {
        int flags = O_WRONLY | O_CREAT;
        if (_append)
            flags |= O_APPEND;
        else
            flags |= O_TRUNC;

        fdOut = open(_outFile, flags, 0644);
        if (fdOut < 0)
        {
            fprintf(stderr, "myshell: %s: %s\n", _outFile, strerror(errno));
            ok = false;
        }
    }

    if (ok && _errFile)
    {
        if (_out_error && fdOut >= 0)
        {
            // >>& so stderr goes to the same place as stdout
            dup2(fdOut, 2);
        }
        else
        {
            int fdErr = open(_errFile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fdErr < 0)
            {
                fprintf(stderr, "myshell: %s: %s\n", _errFile, strerror(errno));
                ok = false;
            }
            else
            {
                dup2(fdErr, 2);
                close(fdErr);
            }
        }
    }
    // something failed, clean up and don't run anything
    if (!ok)
    {
        if (fdIn >= 0)
            close(fdIn);
        if (fdOut >= 0)
            close(fdOut);
        dup2(defErr, 2);
        close(defIn);
        close(defOut);
        close(defErr);
        clear();
        return;
    }
    // one process for each simple command
    std::vector<pid_t> pids;
    int curIn = (fdIn >= 0) ? fdIn : dup(defIn);

    for (int i = 0; i < _numberOfSimpleCommands; i++)
    {
        // this command reads from curIn
        dup2(curIn, 0);
        close(curIn);

        int nextIn = -1;

        if (i == _numberOfSimpleCommands - 1)
        {
            // last command, write to the file or to the terminal
            if (fdOut >= 0)
            {
                dup2(fdOut, 1);
                close(fdOut);
                fdOut = -1;
            }
            else
            {
                dup2(defOut, 1);
            }
        }
        else
        {
            // not the last one, write into a pipe, the next command reads from it
            int p[2];
            if (pipe(p) < 0)
            {
                perror("myshell: pipe");
                break;
            }
            dup2(p[1], 1);
            close(p[1]);
            nextIn = p[0];
        }
        pid_t pid = fork();
        if (pid < 0)
        {
            perror("myshell: fork");
            if (nextIn >= 0)
                close(nextIn);
            break;
        }
        if (pid == 0)
        {
            // child: Ctrl-C should kill it, and close what it doesn't need
            signal(SIGINT, SIG_DFL);
            close(defIn);
            close(defOut);
            close(defErr);
            if (fdOut >= 0)
                close(fdOut);
            if (nextIn >= 0)
                close(nextIn);

            SimpleCommand *sc = _simpleCommands[i];
            execvp(sc->_arguments[0], sc->_arguments);

            // we only get here if execvp failed
            if (errno == ENOENT)
                fprintf(stderr, "myshell: %s: command not found\n", sc->_arguments[0]);
            else
                fprintf(stderr, "myshell: %s: %s\n", sc->_arguments[0], strerror(errno));
            _exit(127);
        }

        pids.push_back(pid);
        curIn = nextIn;
    }
    // put the shell's stdin/stdout/stderr back
    dup2(defIn, 0);
    dup2(defOut, 1);
    dup2(defErr, 2);
    close(defIn);
    close(defOut);
    close(defErr);
    if (fdOut >= 0)
        close(fdOut);

    // wait for the children, unless the command ends with &
    if (!_background)
    {
        for (size_t k = 0; k < pids.size(); k++)
        {
            int status;
            pid_t r = waitpid(pids[k], &status, 0);
            if (r > 0)
                logChild(r, status);
        }
    }

    clear();
}

void Command::prompt()
{
    printf("myshell>");
    fflush(stdout);
}
Command Command::_currentCommand;
SimpleCommand *Command::_currentSimpleCommand;

int main()
{
    // the shell must not die on Ctrl-C
    signal(SIGINT, SIG_IGN);

    logFd = open("shell.log", O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (logFd < 0)
        perror("myshell: cannot open shell.log");

    signal(SIGCHLD, childHandler);

    std::string input;
    Command::_currentCommand.prompt();
    while (std::getline(std::cin, input))
    {
        std::vector<Token> tokens = tokenize(input);
        parse(tokens);
        Command::_currentCommand.prompt();
    }
    printf("\n");
    return 0;
}
