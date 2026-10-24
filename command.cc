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


static bool isWord(TokenType t)
{
    return t == TOKEN_COMMAND || t == TOKEN_ARGUMENT;
}

// free the old string (if any) and store a copy of the new one
static void setStr(char *&dst, const char *src)
{
    if (dst)
        free(dst);
    dst = strdup(src);
}

static void freeSimple(SimpleCommand *sc)
{
    for (int i = 0; i < sc->_numberOfArguments; i++)
        free(sc->_arguments[i]);
    free(sc->_arguments);
    delete sc;
}

static void syntaxError(const char *msg, SimpleCommand *sc)
{
    fprintf(stderr, "myshell: syntax error: %s\n", msg);
    freeSimple(sc);
    Command::_currentCommand.clear();
}

// The tokenizer splits on spaces, so "Hello, World!" arrives as two tokens:
//   "Hello,   and   World!"
// This glues them back into one argument and removes the quotes.
static bool mergeQuotes(const std::vector<Token> &in, std::vector<Token> &out)
{
    for (size_t i = 0; i < in.size(); i++)
    {
        const Token &t = in[i];

        if (t.type != TOKEN_EOF && !t.value.empty() &&
            (t.value[0] == '"' || t.value[0] == '\''))
        {
            char q = t.value[0];
            std::string s = t.value;
            size_t j = i;

            // keep appending tokens until we find the closing quote
            while (s.find(q, 1) == std::string::npos)
            {
                if (j + 1 >= in.size() || in[j + 1].type == TOKEN_EOF)
                    return false; // unterminated quote
                j++;
                s += " " + in[j].value;
            }

            size_t end = s.find(q, 1);
            std::string word = s.substr(1, end - 1) + s.substr(end + 1);
            out.push_back({TOKEN_ARGUMENT, word});
            i = j;
        }
        else
        {
            out.push_back(t);
        }
    }
    return true;
}

void parse(std::vector<Token> &rawTokens)
{
    std::vector<Token> tokens;
    if (!mergeQuotes(rawTokens, tokens))
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
            sc->insertArgument(strdup(t.value.c_str()));
            afterPipe = false;
            break;

        case TOKEN_REDIRECT:
        case TOKEN_APPEND:
        case TOKEN_INPUT:
        case TOKEN_ERROR:
        case TOKEN_REDIRECT_AND_ERROR:
        {
            if (i + 1 >= tokens.size() || !isWord(tokens[i + 1].type))
                return syntaxError("missing file name after redirection", sc);

            const char *file = tokens[++i].value.c_str();

            if (t.type == TOKEN_INPUT)
            {
                setStr(cmd._inputFile, file);
            }
            else if (t.type == TOKEN_ERROR)
            {
                setStr(cmd._errFile, file);
            }
            else if (t.type == TOKEN_REDIRECT)
            {
                setStr(cmd._outFile, file);
                cmd._append = 0;
            }
            else if (t.type == TOKEN_APPEND)
            {
                setStr(cmd._outFile, file);
                cmd._append = 1;
            }
            else // TOKEN_REDIRECT_AND_ERROR  (>>&)
            {
                setStr(cmd._outFile, file);
                setStr(cmd._errFile, file);
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
                    freeSimple(sc); // user just pressed Enter
                    return;
                }
                return syntaxError("missing command", sc);
            }

            cmd.insertSimpleCommand(sc);
            cmd.execute();
            return;
        }
    }

    freeSimple(sc); // not reached, tokens always end with EOF
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

    // print();   // command table (Part 1) - uncomment to debug

    // save the real stdin/stdout/stderr so we can restore them afterwards
    int defIn = dup(0), defOut = dup(1), defErr = dup(2);

    int fdIn = -1, fdOut = -1;
    bool ok = true;

    // ---- open redirection files ----
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
        int flags = O_WRONLY | O_CREAT | (_append ? O_APPEND : O_TRUNC);
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
            dup2(fdOut, 2); // >>&  : stderr goes where stdout goes
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

    if (!ok)
    {
        if (fdIn >= 0) close(fdIn);
        if (fdOut >= 0) close(fdOut);
        dup2(defErr, 2);
        close(defIn);
        close(defOut);
        close(defErr);
        clear();
        return;
    }

    // ---- create the process chain ----
    std::vector<pid_t> pids;
    int curIn = (fdIn >= 0) ? fdIn : dup(defIn);

    for (int i = 0; i < _numberOfSimpleCommands; i++)
    {
        dup2(curIn, 0); // this command reads from curIn
        close(curIn);

        int nextIn = -1;
        if (i == _numberOfSimpleCommands - 1)
        {
            // last command: write to the output file or the terminal
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
            // not last: write into a pipe, the next command reads from it
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
            if (nextIn >= 0) close(nextIn);
            break;
        }

        if (pid == 0)
        {
            // child
            signal(SIGINT, SIG_DFL);
            close(defIn);
            close(defOut);
            close(defErr);
            if (fdOut >= 0) close(fdOut);
            if (nextIn >= 0) close(nextIn);

            SimpleCommand *sc = _simpleCommands[i];
            execvp(sc->_arguments[0], sc->_arguments);

            // only reached if execvp failed
            if (errno == ENOENT)
                fprintf(stderr, "myshell: %s: command not found\n", sc->_arguments[0]);
            else
                fprintf(stderr, "myshell: %s: %s\n", sc->_arguments[0], strerror(errno));
            _exit(127);
        }

        pids.push_back(pid);
        curIn = nextIn;
    }

    // ---- restore the shell's own stdin/stdout/stderr ----
    dup2(defIn, 0);
    dup2(defOut, 1);
    dup2(defErr, 2);
    close(defIn);
    close(defOut);
    close(defErr);
    if (fdOut >= 0) close(fdOut);

    // ---- wait (unless it is a background command) ----
    if (!_background)
    {
        for (size_t k = 0; k < pids.size(); k++)
            waitpid(pids[k], NULL, 0);
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
