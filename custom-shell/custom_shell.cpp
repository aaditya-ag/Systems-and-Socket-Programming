/*
 * Authors - Aaditya Agrawal + Deep Majumder
 * Roll - 19CS10003 + 19CS30015
 * Description - Implementing our own shell
 * Operating System Lab
*/

#include <iostream>
#include <vector>
#include <fstream>
#include <string>
#include <sstream>
#include <set>
#include <deque>
#include <termios.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <poll.h>
#include <unistd.h>
#include <cstring>
#include <cstdlib>
#include <stdio.h>
#include <errno.h>
#include <dirent.h>
using namespace std;

#define BACKSPACE 127
#define MAXHISTSIZE 10000
#define MAX_BUFFER_SIZE 1024
#define ERROR -1
#define OK 0
#define MAX_DIR_LEN 500

/* ******** Class Command ************/
class Command {
public:
    string enteredCmd;
    bool isPipe;
    bool isBackground;
    bool isComposite;
    int inFile;
    int outFile;
    string infilename;
    string outfilename;
    vector<Command*> pipeCmds;
    vector<Command*> bgCmds;
    vector<char*> tokens;
    Command(string _enteredCmd) {
        enteredCmd = string(_enteredCmd);
        isComposite = false;
        isBackground = false;
        isPipe = false;
        inFile = STDIN_FILENO;
        outFile = STDOUT_FILENO;
        infilename = "";
        outfilename = "";
    }
};
/* ***********************************/

/* ******** Command Reading Utilities ************/
string ltrim(const string &s);
string rtrim(const string &s);
string trim(const string &s);
int autocomplete(string &);
string readInteger();
void readLine(string &);
void tokenizeCommand(string &, vector<char *> &);
int splitPipe(string &, vector<string> &);
void searchHistory();
string readInteger();
int autocomplete(string& line);
int splitAmpersand(string str, vector<string>& command, string del = "&");
/* ***********************************************/

/* ********* Signal Handler Utility **************/
void sigchldHandler(int );
/* ***********************************************/

/* ****** Toggle Raw Mode in Terminal ************/
void disableRawMode();
void enableRawMode();
/* ***********************************************/

/* ************ Command Parsers ******************/
int parseCommand(string enteredcmd, Command* cmd, int pipeNum = -1);
int parseCommand(string entered_cmd, vector<Command*>& watchedCmds);
/* ***********************************************/

/* ********** Execution Functions ****************/
int executeSimpleCommand(Command* cmd, int inFD, int outFD, int mw_mode = 0);
int executePipe(Command* cmd, int mw_mode = 0);
int executeCommand(Command* cmd, int mw_mode = 0);
int handleMultiwatch(string entered_cmd);
/* ***********************************************/

/* ****** Command History Utilities **************/
int loadHistory(deque<string>& history);
int saveHistory(deque<string>& history);
int lcs(const string &X, const string &Y);
void searchHistory();
/* ***********************************************/


pid_t parent;
pid_t shell_pgid;
struct termios shell_tmodes;
int shell_terminal;
deque<string> history;
char MASTER_PATH[MAX_DIR_LEN];
const string WHITESPACE = " \n\r\t\f\v";
struct termios original;

int main () {
    getcwd(MASTER_PATH, MAX_DIR_LEN);
    enableRawMode();
    // cout << "     \n\
    //         O   \n\
    //        \\|/  \n\
    //         |   \n\
    //        / \\ \n\
    // Wake up Neo...\n";
    // waitingFor.push_back(-1);
    string enteredcmd;

    loadHistory(history);
    parent = getpid();

    shell_terminal = STDIN_FILENO;
    while (tcgetpgrp (shell_terminal) != (shell_pgid = getpgrp ()))
    kill (- shell_pgid, SIGTTIN);

    signal (SIGINT, SIG_IGN);
    signal (SIGQUIT, SIG_IGN);
    signal (SIGTSTP, SIG_IGN);
    signal (SIGTTIN, SIG_IGN);
    signal (SIGTTOU, SIG_IGN);
    signal (SIGCHLD, sigchldHandler);

    shell_pgid = getpid ();
    if (setpgid (shell_pgid, shell_pgid) < 0) {
        cerr<<"ERROR:: setpgid() failed."<<endl;
        exit (1);
    }

    tcsetpgrp (shell_terminal, shell_pgid);

    tcgetattr (shell_terminal, &shell_tmodes);

    while(1) {
        readLine(enteredcmd);
        if (enteredcmd.empty()) {
			// cout << "You have not entered anything!\n";
			continue;
		}
        
        if(history.empty() || (!history.empty() && enteredcmd.compare(history[history.size()-1]) != 0)) {
            if(history.size() >= MAXHISTSIZE) {
                history.pop_front();
            }
            history.push_back(enteredcmd);
        }
        if (enteredcmd.compare("exit") == 0) {
			cout << "Exiting...\n";
			break;
		}
        if (enteredcmd.compare("history") == 0) {
            // cout << "Showing history..." << endl;
            for(string x: history) {
                cout<<x<<endl;
            }
            continue;
        }
        stringstream ss(enteredcmd);
        string cmdName; ss>>cmdName;
        if(cmdName.compare("multiwatch") == 0) {
            // cout<<"Starting multiwatch..."<<endl;
            if(handleMultiwatch(enteredcmd) < 0) {
                cerr<<"Multiwatch failed"<<endl;
            }
            continue;
        }
        
        Command* cmd = new Command(enteredcmd);
        if(parseCommand(cmd->enteredCmd, cmd) < 0) {
            cerr<<"ERROR:: cannot parse command. please check syntax."<<endl;
            continue;
        }

        if(executeCommand(cmd) < 0) {
            continue;
        }
    }

    saveHistory(history);

	disableRawMode();

    return 0;
}


string ltrim(const string &s) {
    size_t start = s.find_first_not_of(WHITESPACE);
    return (start == string::npos) ? "" : s.substr(start);
}
string rtrim(const string &s) {
    size_t end = s.find_last_not_of(WHITESPACE);
    return (end == string::npos) ? "" : s.substr(0, end + 1);
}
string trim(const string &s) {
    return rtrim(ltrim(s));
}

void disableRawMode() {
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &original);
}

void enableRawMode() {
	tcgetattr(STDIN_FILENO, &original);
	// atexit(disableRawMode);
	struct termios raw = original;
	raw.c_lflag &= ~(ECHO | ICANON | IEXTEN);
    // raw.c_cc[VSUSP] = _POSIX_VDISABLE;
    raw.c_cc[VREPRINT] = _POSIX_VDISABLE;
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}


void readLine(string& line) {
	// clear any buffer present in line from earlier operations
    cout << "neoSH:~$" << MASTER_PATH <<" >>> ";
	line.clear();
	char ch;

    int askedHistoryIndex = history.size();
    int historyLen = history.size();

	while (true) {
		ch = getchar();
		if (ch == BACKSPACE) {
			// Backspace
			if (line.empty()) {
				continue;
			}
			cout << "\b \b"; //Cursor moves 1 position backwards
			if (!line.empty()) {
				line.pop_back();
			}
		} else if (ch == '\t') {
			// Autocomplete
            int status = autocomplete(line);
			if (status == ERROR) {
				// In case of failure
                line.clear();
				cout << ">>> ";
			}
		}
        else if (iscntrl(ch) && ch == 18) {
            // CTRL R pressed
            // search
            while(!line.empty()){
                line.pop_back();
    			cout << "\b \b";
            }
            searchHistory();
            break;
        }
		else if(ch==27){
			// Any Escape Sequence like arrows
			int count = 0;
			bool isUpArrow = true, isDownArrow = true;
			while((ch = getchar())!=EOF){
				count += 1;
				// cout<<"\nRead :"<<ch<<endl;
				// Not an arrow
				if(count == 1 && ch != '[' ){
					isUpArrow = false;
					isDownArrow = false;
				}
				if( count == 2){
					if(ch!='A'){
						isUpArrow = false;
					}	
					if(ch!='B'){
						isDownArrow = false;
					}
				}
				if(count >=2){
					break;
				}
			}
			// cout<<"Was it up? "<<isUpArrow<<endl;
			// cout<<"Was it down? "<<isDownArrow<<endl;
            
            if(!isUpArrow && !isDownArrow){
                continue;
            }

			if(isUpArrow){
                if(askedHistoryIndex <= 0){
                    continue;
                }
                askedHistoryIndex -= 1;
			}else if(isDownArrow){
                if(askedHistoryIndex >= historyLen - 1){
                    continue;
                }
                askedHistoryIndex += 1;
			}

            while(!line.empty()){
                line.pop_back();
                cout<<"\b \b";
            }
            // cout<<"Asked "<<askedHistoryIndex<<endl;
            line = history[askedHistoryIndex];
            cout<<line;
		}
		else  if (ch == '\n') {
			// Newline
			cout << "\n";
			return;
		} else {
			// Anything else
			line += ch;
			cout << ch;
		}
	}
}

int tokenizeCommand(string& command, Command* cmd, int pipenum){
	// cout<<"Received : "<<command<<endl;
	int len = command.length();
	string token = "";

	int head = -1, tail = 0;
	bool foundQuote=false;
	char activeQuote = '\0';
	char singleQuote='\'', doubleQuote='\"';
    vector<string> args;
	// 2 pointer algorithm to tokenize a command
	while(tail < len){
		while(head + 1<len && (foundQuote || command[head+1] != ' ')){
			head += 1;
			token += command[head];
			if(command[head] == singleQuote || command[head] == doubleQuote){
				if(!foundQuote){
					foundQuote = true;
					activeQuote = command[head];
				} else if(activeQuote == command[head]){
					foundQuote = false;
					activeQuote = '\0';
				}
			} 
		}

		if(head >= tail){
            args.push_back(token);
			token.clear();
			tail = head + 1;
			head = tail - 1;
		}else{
			tail += 1;
			head = tail - 1;
		}
	}

	if(foundQuote != false){
		cerr<<"ERROR:: Invalid Command\n";
		cmd->tokens.clear();
        return -1;
		// exit(EXIT_FAILURE);
	}

    vector<string> args_cmd;
    for(int i = 0; i < args.size(); i++) {
        if(args[i] == ">")
        {
            // cout<<"o redirect"<<endl;
            if(i == args.size()-1) {
                cerr<<"ERROR:: argument absent for i/o redirection token."<<endl;
                return -1;
            }
            if(pipenum != -2 && pipenum != -1) {
                cerr<<"WARNING:: output redirection except for final command in a pipe will be ignored."<<endl;
            }
            // else 
            cmd->outfilename = args[i+1];
            i++;
        }
        else if(args[i] == "<")
        {
            // cout<<"i redirect"<<endl;
            if(i == args.size()-1) {
                cerr<<"ERROR:: argument absent for i/o redirection token."<<endl;
                return -1;
            }
            if(pipenum != 0 && pipenum != -1) {
                 cerr<<"WARNING:: input redirection except for first command in a pipe will be ignored."<<endl;
            }
            // else 
            cmd->infilename = args[i+1];
            i++;
        }  
        else {
            args_cmd.push_back(args[i]);
        } 
    }

    for(string s : args_cmd) {
        char* token_c_str = new char[s.length()+1];
        strcpy(token_c_str, const_cast<char *>(s.c_str()));
        cmd->tokens.push_back(token_c_str);
        // cout<<"after io redirection : "<<s<<endl;
    }

	cmd->tokens.push_back(NULL);
    return 0;
}

string readInteger() {
	string num = "";
	char ch;
	while (true) {
		ch = getchar();
		if (ch == '\n') {
			cout << endl;
			break;
		} else if ( ch == '\t') {
			continue;
		} else if (ch == BACKSPACE) {
			if (num.empty()) {
				continue;
			}
			cout << "\b \b"; //Cursor moves 1 position backwards
			num.pop_back();
		} else {
			cout << ch;
			num += ch;
		}

	}
	return num;
}

int autocomplete(string& line) {
	vector<string> dirItems;
	struct dirent *de;  // Pointer for directory entry

	// opendir() returns a pointer of DIR type.
	DIR *dr = opendir(".");

	if (dr == NULL)  // opendir returns NULL if couldn't open directory
	{
		cerr << "opendir error(): Could not open current directory for autocompletion" << endl;
		return ERROR;
	}

	// for readdir()
	// Excluding '.' and '..' for autocompletion
	while ((de = readdir(dr)) != NULL) {
		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
			continue;
		}
		dirItems.push_back(string(de->d_name));
	}

	int last = line.length() - 1;
	while (last >= 0) {
		if (line[last] == ' ') {
			break;
		}
		last--;
	}

	int lengthOfLastToken = line.length() - 1 - last;
	string lastToken = line.substr(last + 1, lengthOfLastToken);
	// cout<<"token"<<" "<<lastToken<<endl;

	vector<string> matches;
	int maxMatchLength = -1;
	for (auto s : dirItems) {
		int matchLen = 0;
		for (int i = 0; i < min(s.length(), lastToken.length()); i++) {
			if (s[i] != lastToken[i]) {
				break;
			}
			matchLen += 1;
		}

		if (matchLen > maxMatchLength && matchLen > 0) {
			maxMatchLength = matchLen;
			matches.clear();
			matches.push_back(s);
		} else if (matchLen == maxMatchLength) {
			matches.push_back(s);
		}
	}

	// cout<<"\nSize => "<<matches.size()<<endl;
	// cout<<"Len => "<<maxMatchLength<<endl;

	if (matches.empty()) {
		return OK;
	} else if (matches.size() == 1) {

		while (lastToken.length() > maxMatchLength) {
			lastToken.pop_back();
			line.pop_back();
			cout << "\b \b";
		}

		while (lastToken.length() < matches[0].length()) {
			int index = lastToken.length();
			char ch = matches[0][index];
			lastToken += ch;
			line += ch;
			cout << ch;
		}
	} else {
		cout << endl;
		cout << "Displaying " << matches.size() << " Possibilities" << endl;
		cout << "Press the corresponding serial number to autocomplete" << endl;
		for (int i = 0; i < matches.size(); i++) {
			cout << i + 1 << ". " << matches[i] << endl;
		}

		cout << "Enter your choice: ";
		string num = readInteger();

		int userChoice = -1;
		try {
			userChoice = stoi(num);
		} catch (const std::exception& e) {
			// do nothing
		}

		if (userChoice < 1 || userChoice > matches.size() ) {
			cout << "Your choice is invalid" << endl;
			line.clear();
			return ERROR;
		}

		cout << ">>> " << line;
		userChoice -= 1;

		while (lastToken.length() > maxMatchLength) {
			lastToken.pop_back();
			line.pop_back();
			cout << "\b \b";
		}

		while (lastToken.length() < matches[userChoice].length()) {
			int index = lastToken.length();
			char ch = matches[userChoice][index];
			lastToken += ch;
			line += ch;
			cout << ch;
		}
	}

	return OK;
}

int splitPipe(string& line, vector<string>& commandList) {
	string command = "";
	int numPipes = 0;

	int len = line.length();
	for(int i=0;i < len;i++){

		if(line[i] == ' ' && command.empty()){
			continue;
		} else if(line[i] == '|'){
			++numPipes;
			while(!command.empty() && command.back() == ' '){
				command.pop_back();
			}
			commandList.push_back(command);
			command.clear();
		} else {
			command += line[i];
		}

		if(i == len-1){
			// Reached end of string
			if(!command.empty()){
				while(!command.empty() && command.back() == ' '){
					command.pop_back();
				}
				commandList.push_back(command);
			}
			command.clear();
		}
	}

	return numPipes;
}


int splitAmpersand(string str, vector<string>& command, string del) {
    int count = 0;
    size_t pos = 0;
    string token;
    while ((pos = str.find(del)) != string::npos) {
        count ++;
        token = str.substr(0, pos+1);
        command.push_back(trim(token));
        str.erase(0, pos + del.length());
    }
    if(trim(str).length() > 0) {
        count++;
        command.push_back(trim(str));
    }
    return count;
}

int parseCommand(string enteredcmd, Command* cmd, int pipeNum) {
    // utility for parsing mixed ampersand commands
    trim(enteredcmd);
    int len = enteredcmd.length();

    vector<string> bgcomm;
    int bg_count = splitAmpersand(enteredcmd, bgcomm);
    //cout<<bg_count<<endl;
    //return -1;

    if(bg_count > 1) {
        cmd->isComposite = true;
        for(auto s : bgcomm) {
            cmd->bgCmds.push_back(new Command(s));
            parseCommand(s, cmd->bgCmds[cmd->bgCmds.size()-1]);
        }
    }
    else {
        cmd->isComposite = false;
        if(len > 0 && enteredcmd[len-1] == '&') {
            cmd->isBackground = true;
            cmd->enteredCmd.pop_back();
            enteredcmd.pop_back();
        }

        vector<string> pipecomm;
        int pipe_count = splitPipe(enteredcmd, pipecomm);
        if(pipe_count > 0) {
            cmd->isPipe = true;
            for(int i = 0; i < pipecomm.size(); i++) {
                string s = pipecomm[i];
                //cout<<s<<endl;
                cmd->pipeCmds.push_back(new Command(s));
                if(i == 0) 
                    parseCommand(s, cmd->pipeCmds[cmd->pipeCmds.size()-1], 0);
                else if(i == pipecomm.size()-1) 
                    parseCommand(s, cmd->pipeCmds[cmd->pipeCmds.size()-1], -2);
                else 
                    parseCommand(s, cmd->pipeCmds[cmd->pipeCmds.size()-1], i);
                if(cmd->isBackground) {
                    cmd->pipeCmds[cmd->pipeCmds.size()-1]->isBackground = true;
                }
            }
            cmd->infilename = cmd->pipeCmds[0]->infilename;
            cmd->outfilename = cmd->pipeCmds[cmd->pipeCmds.size()-1]->outfilename;
        }
        else {
            cmd->isPipe = false;
            if( tokenizeCommand(cmd->enteredCmd, cmd, pipeNum) < 0 ) {
                return -1;
            }
        }    
    }    
    return 0;
}


int parseCommand(string entered_cmd, vector<Command*>& watchedCmds) {
    string cmd = trim(entered_cmd);
    int closeSqB = cmd.find(']');
    if(closeSqB == string::npos) {
        cerr<<"ERROR:: incorrect multiwatch syntax, missing \']\'"<<endl;
        return -1;
    }
    int nextcloseSqB = cmd.find(']', closeSqB+1);
    if(nextcloseSqB != string::npos) {
        cerr<< "ERROR:: incorrect multiwatch syntax, extra \']\'"<<endl;
        return -1;
    }
    int openSqB = cmd.find('[');
    if(openSqB == string::npos) {
        cerr<<"ERROR:: incorrect multiwatch syntax, missing \'[\'"<<endl;
        return -1;
    }
    int nextopenSqB = cmd.find('[', openSqB+1);
    if(nextopenSqB != string::npos) {
        cerr<< "ERROR:: incorrect multiwatch syntax, extra \'[\'"<<endl;
        return -1;
    }
    if(openSqB >= closeSqB) {
        cerr<< "ERROR:: incorrect multiwatch syntax, \']\' precedes \'[\'"<<endl;
        return -1;
    }

    string cmdliststr = cmd.substr(openSqB+1,closeSqB-openSqB-1);
    // cout<<cmdliststr<<endl;

    vector<string> cmdlist;
    splitAmpersand(cmdliststr, cmdlist, ",");

    for(auto s:cmdlist) {
        if((*(s.begin()) != '\"')&&(*(s.end()-1) != '\"')) {
            cerr<< "ERROR:: incorrect multiwatch syntax"<<endl;
            return -1;
        }
        else {
            // cout<<">"<<s<<"<"<<endl;
            string cmdstr ;
            if(s == *(cmdlist.end()-1))
                cmdstr = s.substr(1,s.length()-2);
            else 
                cmdstr = s.substr(1,s.length()-3);
            // cout<<"cmd:"<<cmdstr<<endl;
            Command* nextcmd = new Command(cmdstr);
            if(parseCommand(nextcmd->enteredCmd, nextcmd) < 0) {
                cerr<<"WARNING:: skipping cmd["<<cmdstr<<"]"<<endl;
                continue;
            } 
            nextcmd->isBackground = true;
            watchedCmds.push_back(nextcmd);
        }
    }
    return 0;
}



void sigchldHandler(int signum) {
    int pid, status, serrno;
    // serrno = errno;
    while (1)
    {
        pid = waitpid (WAIT_ANY, &status, WNOHANG);
        if (pid < 0) {
            // perror ("waitpid");
            break;
        }
        if (pid == 0)
            break;
        cout<<"Completed pid["<<pid<<"]."<<endl;
    }
    // errno = serrno;
}

int executeSimpleCommand(Command* cmd, int inFD, int outFD, int mw_mode) {
    // cout<<"Simple CMD : "<<cmd->enteredCmd<<endl;
    if(cmd->tokens.size() > 0 && strcmp(cmd->tokens[0],"cd") == 0 ){
       if(cmd->tokens.size() >= 2 && chdir(cmd->tokens[1])==0){
           getcwd(MASTER_PATH,MAX_DIR_LEN);
       }else{
           cout<<"Directory not found!"<<endl;
       }
       return 0;
   }

    pid_t pid, pgid = 0;
    pid = fork();
    if(pid == 0) {
        if(!mw_mode) {
            pid = getpid();
            if(pgid == 0) pgid = pid;
            setpgid(pid, pgid);
        }
        // cout<<cmd->isBackground<<endl;
        if(!cmd->isBackground) {
            disableRawMode();
            tcsetpgrp(shell_terminal, pgid);
        }
        signal (SIGINT, SIG_DFL);
        signal (SIGQUIT, SIG_DFL);
        signal (SIGTSTP, SIG_DFL);
        signal (SIGTTIN, SIG_DFL);
        signal (SIGTTOU, SIG_DFL);
        signal (SIGCHLD, SIG_DFL);
        if(inFD != STDIN_FILENO) {
            dup2(inFD, STDIN_FILENO);
            close(inFD);
        }
        // cout<<"DUPIN"<<endl;
        if(outFD != STDOUT_FILENO) {
            dup2(outFD, STDOUT_FILENO);
            close(outFD);
        }
        // cout<<"DUPOUT"<<endl;
        int status = execvp(cmd->tokens[0], cmd->tokens.data());
        if(status == -1) {
            cerr<<"ERROR:: command not found."<<endl;
        }
        exit(status);
    }
    else {
        if(!mw_mode) {
            if(!pgid) pgid = pid;
            setpgid(pid, pgid);
        }
    }

    if(!cmd->isBackground) {
        tcsetpgrp(shell_terminal, pgid);
        int status;
        pid_t x = waitpid(pid, &status, WUNTRACED);
        // cout<<"Wait Status : "<<status<<endl;
        // cout<<"Waiting stopped.."<<endl;
        if(!WIFEXITED(status) && WIFSTOPPED(status)) {
            // cout<<"Stopped by : "<<WSTOPSIG(status)<<endl;
            // cout<<"Resuming child["<<pid<<"] now..."<<x<<endl;
            if(kill(-pid, SIGCONT) < 0) {
                cerr<<"ERROR:: transmission of SIGCONT failed."<<endl;
            }
            cout<<endl<<"Pushed ["<<cmd->enteredCmd<<"] to background : pid["<<pid<<"]"<<endl;
        }
        tcsetpgrp(shell_terminal, shell_pgid);
        struct termios job_tmodes;
        tcgetattr(shell_terminal, &job_tmodes);
        tcsetattr(shell_terminal, TCSADRAIN, &shell_tmodes);    
    }
    else{
        if(!mw_mode) {
            int status;
            cout<<"Launched ["<<cmd->enteredCmd<<"] in background : pid["<<pid<<"]"<<endl;
            // waitpid(pid, &status, WNOHANG|WUNTRACED);
            // if(WIFEXITED(status)) {
            //     cout<<"Completed ["<<cmd->enteredCmd<<"] in background : pid["<<pid<<"]"<<endl;
            // }

        }
        // cout<<"Background execution"<<endl;
    }
    return 0;
}

int executePipe(Command* cmd, int mw_mode) {
    int numArgs = cmd->tokens.size()-1;

    if(cmd->infilename.length() > 0 && ((cmd->inFile = open(cmd->infilename.c_str(), O_RDONLY)) < 0)) {
        cerr<<"WARNING:: i/o redirection file cannot be accessed... using stdi/o."<<endl;
        cmd->inFile = STDIN_FILENO;
    }

    if(cmd->outfilename.length() > 0 && ((cmd->outFile = open(cmd->outfilename.c_str(), O_WRONLY|O_CREAT, 0666)) < 0)){
        cerr<<"WARNING:: i/o redirection file cannot be accessed... using stdi/o."<<endl;
        cmd->outFile = STDOUT_FILENO;
    }

    if(cmd->isPipe) {
        // Implement piped executions
        int status = 1;
        // cout<<"pipe commands"<<endl;
        int pipeFD[2];
        int inFD = cmd->inFile;
        int i = 0;
        int pipeErr = 0;
        for(i = 0; i < cmd->pipeCmds.size() - 1; i++ ) {
            if(pipe(pipeFD) == -1) {
                cerr<<"ERROR:: pipe() failed."<<endl;
                pipeErr = 1;
            } else {
                status = executeSimpleCommand(cmd->pipeCmds[i], inFD, pipeFD[1], mw_mode);
                close(pipeFD[1]);
                inFD = pipeFD[0];
            }
        }
        // put not error later
        if(!pipeErr) {
            //cout<<"And finish."<<endl;
            //cout<<cmd->pipeCmds[i]->outfilename<<endl;
            status = executeSimpleCommand(cmd->pipeCmds[i], inFD, cmd->outFile, mw_mode);
        }
        return status;
    } 
    else {
        return executeSimpleCommand(cmd, cmd->inFile, cmd->outFile, mw_mode);
    }

}

int executeCommand(Command* cmd, int mw_mode) {
    if(cmd->isComposite) {
        int i = 0;
        int status = 1;
        for(int i = 0; i < cmd->bgCmds.size(); i++) {
            status = executePipe(cmd->bgCmds[i], mw_mode);
        }
        return status;
    }
    else {
        return executePipe(cmd, mw_mode);
    }
}


int handleMultiwatch(string entered_cmd) {
    pid_t surrogate, surrogate_grp = 0;
    surrogate = fork();
    if(surrogate == 0) {
        surrogate = getpid();
        if(!surrogate_grp) surrogate_grp = surrogate;
        setpgid(surrogate, surrogate_grp);
        tcsetpgrp(shell_terminal, surrogate_grp);
        signal(SIGINT, SIG_DFL);
        signal(SIGCHLD, SIG_DFL);
        
        vector<Command*> watchedCmds;
        // cout<<entered_cmd<<endl;
        
        if(parseCommand(entered_cmd, watchedCmds)<0) {
            exit(-1);
        }

        int numWatched = watchedCmds.size();
        // cout<<numWatched<<endl;

        vector<int> writefds;
        vector<int> readfds;
        
        int pipes[numWatched][2];
        for(int i = 0; i < numWatched; i++) {
            if(pipe(pipes[i]) < 0) {
                cerr<<"ERROR:: pipe() failed."<<endl;
                exit(-1);
            }
            int fd = pipes[i][1];
            writefds.push_back(fd);

            int rfd = pipes[i][0];
            readfds.push_back(rfd);
        }
        vector<pid_t> processes;
        for(int i = 0; i < numWatched; i++) {
            pid_t pid;
            pid = fork();
            if(pid == 0) {
                dup2(writefds[i], STDOUT_FILENO);
                close(writefds[i]);

                executeCommand(watchedCmds[i], 1);
                exit(1);
            } else {
                processes.push_back(pid);
            }
        } 

        int nfds, openfds;
        struct pollfd* pollfdset;
        openfds = nfds = numWatched;

        pollfdset = (struct pollfd*)calloc(nfds, sizeof(struct pollfd));
        if(pollfdset == NULL) {
            cerr<<"ERROR:: calloc() failed."<<endl;
        }

        for(int i = 0; i < nfds; i++) {
            pollfdset[i].fd = pipes[i][0];
            pollfdset[i].events = POLLIN;
        }

        while(openfds > 0) {
            int ready = poll(pollfdset, nfds, -1);
            if(ready == -1) {
                cerr<<"ERROR:: bad poll()."<<endl;
                exit(-1);
            }
            
            for(int j = 0; j < nfds; j++) {
                char buffer[MAX_BUFFER_SIZE];
                if (pollfdset[j].revents != 0) {
                    // printf("  fd=%d; events: %s%s%s\n", pollfdset[j].fd, (pollfdset[j].revents & POLLIN)  ? "POLLIN "  : "",(pollfdset[j].revents & POLLHUP) ? "POLLHUP " : "",(pollfdset[j].revents & POLLERR) ? "POLLERR " : "");
                    if (pollfdset[j].revents & POLLIN) {
                        memset(buffer, 0, MAX_BUFFER_SIZE);
                        ssize_t s = read(pollfdset[j].fd, buffer, sizeof(buffer));
                        if (s == -1) {
                            cerr<<"ERROR:: read() failed."<<endl;
                        }
                        cout<<endl;
                        cout<<"\""<<watchedCmds[j]->enteredCmd<<"\", current_time:"<<(unsigned long)time(NULL)<<endl;
                        cout<<"<-<-<-<-<-<-<-<-<-<-<-<-<-<-<-<-<-<-<-"<<endl;
                        cout<<buffer;
                        cout<<"->->->->->->->->->->->->->->->->->->->"<<endl<<endl;
                        // printf("    read %zd bytes: %.*s\n",s, (int) s, buffer);
                    } else {                /* POLLERR | POLLHUP */
                        // printf("    closing fd %d\n", pollfdset[j].fd);
                        if (close(pollfdset[j].fd) == -1) {
                            cerr<<"ERROR:: close() failed."<<endl;
                        }
                        openfds--;
                    }
                }
            }
        }
        
        return 0;
    }
    else {
        if(!surrogate_grp) surrogate_grp = surrogate; 
        setpgid(surrogate, surrogate_grp);

        tcsetpgrp(shell_terminal, surrogate_grp);
        int status;
        pid_t x = waitpid(surrogate, &status, WUNTRACED);
        // cout<<"Wait Status : "<<status<<endl;
        // cout<<"Waiting stopped.."<<endl;
        
        tcsetpgrp(shell_terminal, shell_pgid);
        struct termios job_tmodes;
        tcgetattr(shell_terminal, &job_tmodes);
        tcsetattr(shell_terminal, TCSADRAIN, &shell_tmodes);
        if(WIFEXITED(status)) {
            return WEXITSTATUS(status);
        }
        return 0;
    }
}



int loadHistory(deque<string>& history) {
    string history_file = "./mybash_history";
    ifstream file_in(history_file);
    string line;
    while(file_in) {
        getline(file_in, line);
        if(trim(line).length() > 0)
            history.push_back(trim(line));
    }
    file_in.close();
    return 0;
}

int saveHistory(deque<string>& history) {
    string history_file = "./mybash_history";
    ofstream file_out(history_file);
    for(string x : history) {
        if(x.length() > 0)
            file_out<<x<<endl;   
    }
    file_out.close();
    return 0;
}

int lcs(const string &X, const string &Y) {
    int m = X.length(), n = Y.length();
    int L[2][n + 1];
    bool f;
    int maxlen = 0;
    for (int i = 0; i <= m; i++) {
        f = i & 1;
        for (int j = 0; j <= n; j++) {
            if (i == 0 || j == 0){
                L[f][j] = 0;
            }
            else if (X[i-1] == Y[j-1]) {
              L[f][j] = L[1 - f][j - 1] + 1;
              maxlen = max(maxlen, L[f][j]);
            }else {
                L[f][j] = 0;
            }
        }
    }
    return maxlen;
}

void searchHistory(){
    cout<<"Enter search term: ";
    char ch;
    string line;
    while(true){
        ch = getchar();
        if(ch=='\t' || (iscntrl(ch) && ch==18)){
           continue;
        }
        else if(ch=='\n'){
            cout<<endl;
            break;
        }
        else if(ch == BACKSPACE){
            if (line.empty()) {
				continue;
			}
			cout << "\b \b"; //Cursor moves 1 position backwards
			line.pop_back();
        }
        else{
            cout<<ch;
            line += ch;
        }
    }

    vector<int> lcsArray;
    int maxCommonLen=0;
    for(int i=0;i<history.size();++i){
        string& cmd=history[i];
        int common = lcs(line,cmd);
        lcsArray.push_back(common);
        maxCommonLen = max(maxCommonLen, common);
    }

    set<string> matches;
    for(int i=0;i<history.size();++i){
        if(history[i].length() == lcsArray[i]) {
            matches.insert(history[i]);
        }
        if(lcsArray[i] == maxCommonLen && maxCommonLen > 2){
            matches.insert(history[i]);
        }
    }

    if(matches.empty()){
        cout<<"No match for search term in history"<<endl;
    }else{
        cout<<"#Command(s) found: "<<matches.size() <<endl;
        int count=0;
        for(const string &cmd:matches){
            cout<<count+1<<". "<<cmd<<endl;
            ++count;
        }
    }
}