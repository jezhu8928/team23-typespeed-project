// compile with: clang++ -std=c++20 -Wall -Werror -Wextra -Wpedantic -g3 -o team23-typespeed team23-typespeed.cpp
// run with: ./team23-typespeed 2> /dev/null
// run with: ./team23-typespeed 2> debugoutput.txt
// "2>" redirect standard error (STDERR; cerr)
// /dev/null is a "virtual file" which discard contents
// Works best in Visual Studio Code if you set:
// Settings -> Features -> Terminal -> Local Echo Latency Threshold = -1
// https://en.wikipedia.org/wiki/ANSI_escape_code#3-bit_and_4-bit

#include <iostream>
#include <vector>
#include <string>
#include <random>
#include <map>
#include <chrono>    // for dealing with time intervals
#include <cmath>     // for max() and min()
#include <termios.h> // to control terminal modes
#include <unistd.h>  // for read()
#include <fcntl.h>   // to enable / disable non-blocking read()
#include <fstream>

// Because we are only using #includes from the standard, names shouldn't conflict
using namespace std;

// Constants

// Disable JUST this warning (in case students choose not to use some of these constants)
#pragma clang diagnostic push

#pragma clang diagnostic ignored "-Wunused-const-variable"

const char NULL_CHAR{'z'};
const char UP_CHAR{'w'};
const char DOWN_CHAR{'s'};
const char LEFT_CHAR{'a'};
const char RIGHT_CHAR{'d'};
const char QUIT_CHAR{'q'};
const char FREEZE_CHAR{'f'};
const char CREATE_CHAR{'c'};
const char BLOCKING_CHAR{'b'};
const char COMMAND_CHAR{'o'};

const string ANSI_START{"\033["};
const string START_COLOUR_PREFIX{"1;"};
const string START_COLOUR_SUFFIX{"m"};
const string STOP_COLOUR{"\033[0m"};

const unsigned int COLOUR_IGNORE{0}; // this is a little dangerous but should work out OK
const unsigned int COLOUR_BLACK{30};
const unsigned int COLOUR_RED{31};
const unsigned int COLOUR_GREEN{32};
const unsigned int COLOUR_YELLOW{33};
const unsigned int COLOUR_BLUE{34};
const unsigned int COLOUR_MAGENTA{35};
const unsigned int COLOUR_CYAN{36};
const unsigned int COLOUR_WHITE{37};

const unsigned short MOVING_NOWHERE{0};
const unsigned short MOVING_LEFT{1};
const unsigned short MOVING_RIGHT{2};
const unsigned short MOVING_UP{3};
const unsigned short MOVING_DOWN{4};

const int YELLOW_STAGE_START{30}; //where the words start turning yellow
const int RED_STAGE_START{50};    //col num of where it turns red

//ends the stop warning, so only variables inbetween this pragma and that one
#pragma clang diagnostic pop

struct positionStruct
{
    int row;
    int col;
};

// add speed, moving property later?
struct word
{
    string content;
    positionStruct position;
    bool cleared;
    unsigned int colour;
};

typedef vector<word> wordvector;

struct termios initialTerm;
//generates random seed so words don't come out in the same "random" order each time
random_device randDevice;
unsigned seed = randDevice();
default_random_engine generator(seed);

//uniform_int_distribution is a class whose function gives random probability from range of given numbers
uniform_int_distribution<unsigned int> wordcolour(COLOUR_RED, COLOUR_WHITE);

// Utility Functions

// These two functions are taken from StackExchange and are
// all of the "magic" in this code.

auto SetupScreenAndInput() -> void
{
    struct termios newTerm;
    // Load the current terminal attributes for STDIN and store them in a global
    tcgetattr(fileno(stdin), &initialTerm);
    newTerm = initialTerm;
    // Mask out terminal echo and enable "noncanonical mode"
    // " ... input is available immediately (without the user having to type
    // a line-delimiter character), no input processing is performed ..."
    newTerm.c_lflag &= ~ICANON;
    newTerm.c_lflag &= ~ECHO;
    newTerm.c_cc[VMIN] = 1;

    // Set the terminal attributes for STDIN immediately
    auto result{tcsetattr(fileno(stdin), TCSANOW, &newTerm)};
    if (result < 0)
    {
        cerr << "Error setting terminal attributes [" << result << "]" << endl;
    }
}

auto TeardownScreenAndInput() -> void
{
    // Reset STDIO to its original settings
    tcsetattr(fileno(stdin), TCSANOW, &initialTerm);
}

auto SetNonblockingReadState(bool desiredState = true) -> void
{
    auto currentFlags{fcntl(0, F_GETFL)};
    if (desiredState)
    {
        fcntl(0, F_SETFL, (currentFlags | O_NONBLOCK));
    }
    else
    {
        fcntl(0, F_SETFL, (currentFlags & (~O_NONBLOCK)));
    }
    cerr << "SetNonblockingReadState [" << desiredState << "]" << endl;
}

// Everything from here on is based on ANSI codes
// Note the use of "flush" after every write to ensure the screen updates
auto ClearScreen() -> void { cout << ANSI_START << "2J" << flush; }
auto MoveTo(unsigned int x, unsigned int y) -> void { cout << ANSI_START << x << ";" << y << "H" << flush; }
auto HideCursor() -> void { cout << ANSI_START << "?25l" << flush; }
auto ShowCursor() -> void { cout << ANSI_START << "?25h" << flush; }
auto GetTerminalSize() -> positionStruct
{
    // This feels sketchy but is actually about the only way to make this work
    MoveTo(999, 999);
    cout << ANSI_START << "6n" << flush;
    string responseString;
    char currentChar{static_cast<char>(getchar())};
    cout << currentChar;
    while (currentChar != 'R')
    {
        responseString += currentChar;
        currentChar = getchar();
    }
    // format is ESC[nnn;mmm ... so remove the first 2 characters + split on ; + convert to unsigned int
    responseString.erase(0, 2);
    auto semicolonLocation = responseString.find(";");
    auto rowsString{responseString.substr(0, semicolonLocation)};
    auto colsString{responseString.substr((semicolonLocation + 1), responseString.size())};
    auto rows = stoul(rowsString);
    auto cols = stoul(colsString);
    positionStruct returnSize{static_cast<int>(rows), static_cast<int>(cols)};
    return returnSize;
}

// defaults: foreground white, no background
auto MakeColour(string inputString, const unsigned int foregroundColour = COLOUR_WHITE, const unsigned int backgroundColour = COLOUR_IGNORE) -> string
{
    string outputString;
    outputString += ANSI_START;
    outputString += START_COLOUR_PREFIX;
    outputString += to_string(foregroundColour);
    if (backgroundColour) // evaluates if background colour isn't IGNORE values
    {
        outputString += ";";
        outputString += to_string((backgroundColour + 10)); // Tacky but works
    }
    outputString += START_COLOUR_SUFFIX;
    outputString += inputString;
    outputString += STOP_COLOUR;
    return outputString;
}


auto UpdateWordColumnPositions(wordvector & activeWords) -> void
{
    // Deal with movement commands
    // Update the position of each word
    // Use a reference so that the actual position updates
    for (auto &currentWord : activeWords)
    {
        // temporarily store the column (so we can ensure it's within the boundary)
        int proposedCol{currentWord.position.col};
        int wordLength{static_cast<int>(currentWord.content.length())};
        // just move right, by the same amount each time
        // possibly add speed so it increments by different amount depending on score
        proposedCol += 1;

        //check if word at certain location, if it is change word colour
        if (proposedCol < RED_STAGE_START and proposedCol >= YELLOW_STAGE_START)
        {
            currentWord.colour = COLOUR_YELLOW;
        }
        else if (proposedCol >= RED_STAGE_START)
        {
            currentWord.colour = COLOUR_RED;
        }

        // moves right -- boundary of 70 - wordLength so the ends of the words hit a boundary instead of a beginning
        currentWord.position.col = min((70 - wordLength), proposedCol);
    }
}

// creates word in vector
auto CreateWord(wordvector &activeWords, vector<string> wordList) -> void
{
    // distribution to pick random words from list
    uniform_int_distribution<unsigned int> wordindex(0, wordList.size() - 1);
    // distribution to pick starting row from (limit to box dimensions)
    uniform_int_distribution<int> startrow(2, 31);
    //cerr << "Creating word" << endl;
    // init new words with a random word and start position
    word newWord{
        .content = wordList[wordindex(generator)],
        .position = {startrow(generator), 1},
        .cleared = false,
        .colour = COLOUR_GREEN,
    };
    activeWords.push_back(newWord);
}

// displays all active words
auto DisplayWords(wordvector activeWords) -> void
{
    for (auto currentWord : activeWords)
    {
        MoveTo(currentWord.position.row, currentWord.position.col);
        cout << MakeColour(currentWord.content, currentWord.colour) << flush;
    }
}

// removes words from the wordvector when they reach the boundary
auto ClearWords(wordvector &activeWords, unsigned int &score, unsigned int &lives) -> void
{
    // index never negative so use unsigned
    for (unsigned int i = 0; i < activeWords.size(); i++)
    { // no need to pass by ref (not changing word elements)
        word currentWord = activeWords[i];
        int wordLength{static_cast<int>(currentWord.content.length())};
        if (currentWord.cleared == true)
        {
            //cerr << "erased: " << currentWord.content << endl;
            activeWords.erase(activeWords.begin() + i);
            score += wordLength * 10;
        }
        else if (currentWord.position.col == (70 - wordLength)) // 70 is column boundary, account for different length words
        {                                               
            activeWords.erase(activeWords.begin() + i); // begin + index bc we need to use an iterator
            lives -= 1;
        }
    }
}

// checks if word matches any that already exists
auto CheckMatchedWord(wordvector &activeWords, string attempt, bool & quit) -> void
{
    for (auto &currentWord : activeWords)
    {
        if (attempt == currentWord.content)
        {
            currentWord.cleared = true;
            //cerr << "cleared: " << currentWord.content << endl;
        }
        else if (attempt == "quit")
        {
            quit = true;
        }
        // check for pause here and set boolean (later)
    }
}

// shows current score 
auto ShowStats(unsigned int score, unsigned int lives) -> void
{
    cout << "score: " << score << "\tlives: " << lives << flush;
}

// function to load file
// pass by reference to directly manipulate wordlist vector
auto LoadWordList(string fileName, vector<string> &wordList) -> void
{
    ifstream input(fileName);
    string wordInList;

    while (getline(input, wordInList))
    {
        wordList.push_back(wordInList);
    }

    input.close();
}

// load high score file into map
// pass map by ref to store values in it directly
auto LoadHighScores(string fileName, map<string, unsigned int> &highScores) -> void
{
    // init input stream to read from external file
    ifstream scoreFile(fileName);
    string currentLine;
    string userName;
    int totalLines{0};
    int totalLines = 0;
    while (getline(scoreFile, currentLine))
    { // loop through file
        totalLines += 1;
        if (totalLines % 2 == 1)
        { // odd lines have name
            userName = currentLine;
        }
        else
        {
            unsigned int score = stoul(currentLine); // even lines have score
            highScores[userName] = score;            // add to map
            //cerr << userName << ", " << score;
        }
    }
    // close input stream
    scoreFile.close();
}

// display start menu, allow for user interaction to start/quit game or view rules
auto StartMenu(string &name) -> bool
{
    cout << "*********************" << endl;
    cout << "WELCOME TO TYPESPEED!" << endl;
    cout << "*********************" << endl;
    cout << MakeColour("\ntype \"start\" to start playing, \"rules\" to see rules, or \"quit\" to exit:", COLOUR_CYAN) << endl;

    string command;
    // get command from user input
    cin >> command;
    // loop until user chooses to leave menu or proceed to game
    while (command.compare("start") != 0 and command.compare("quit") != 0)
    {
        if (command.compare("rules") == 0)
        {
            cout << "\nHOW IT WORKS:\n* when you start playing, you'll see words moving across the screen: it's your job to type them in!" << endl;
            cout << "* when you type the word in correctly, you get to clear the word." << endl;
            cout << "* points will be calculated based on the length of the words you clear." << endl;
            cout << "* if you don't clear words before they hit the boundary, you'll lose a life. if you lose 5 lives, the game will end." << endl;
            cout << "\n* once you've typed a character, you can't delete it, so accuracy matters just as much as speed." << endl;
            cout << "\n* if you want to quit at any point, type \"quit\" in. we won't judge you for it." << endl;

            cout << "\nwe hope you enjoy our game! - anna, jessica and tiffany <3" << endl;
        }
        else
        { // input does not match any valid command
            cout << MakeColour("invalid command, try again ;)", COLOUR_RED) << endl;
        }
        cout << MakeColour("\ntype \"start\" to start playing, \"rules\" to see rules, or \"quit\" to exit:", COLOUR_CYAN) << endl;
        cin >> command;
    }
    // user wants to proceed to game
    if (command.compare("start") == 0)
    {
        // get their name to be used in high score list
        cout << MakeColour("\nbefore we begin, what's your name? it'll be saved for high scores :)", COLOUR_MAGENTA) << endl;
        cin >> name;
        return true;
    }
    return false;
}

// displays game over, user score and specific tailored message depending on score
auto ShowEndScreen(unsigned int score, unsigned int scorelevel = 500) -> void
{
    cout << " _____ ____  _      _____   ____  _     _____ ____\n"
         << flush;
    cout << "/  __//  _ \\/ \\__/|/  __/  /  _ \\/ \\ |\\/  __//  __\\\n"
         << flush;
    cout << "| |  _| / \\|| |\\/|||  \\    | / \\|| | //|  \\  |  \\/|\n"
         << flush;
    cout << "| |_//| |-||| |  |||  /_   | \\_/|| \\// |  /_ |    /\n"
         << flush;
    cout << "\\____\\\\_/ \\|\\_/  \\|\\____\\  \\____/\\__/  \\____\\\\_/\\_\\\n\n"
         << flush;
    /* scorelevel is default value because if implemented different levels of play (chosen by player at beginning), 
       scorelevel for a positive message can be adjusted */

    // only display compliment if score over threshold
    if (score > scorelevel)
    {
        cout << "you're a pro 😤\n\n"
             << "score: " << score << "\n"
             << flush;
    }
    else
    {
        cout << "you tried? 😕\n\n"
             << "score: " << score << "\n"
             << flush;
    }
}

// determines whether user achieved a score in the top 3 all time
// then, displays high scores and saves them for future plays
auto ProcessHighScores(string fileName, map<string, unsigned int> &highScores, string userName, unsigned int score) -> void
{
    cout << "\nCURRENT HIGH SCORES" << endl;
    string lowestUser;
    unsigned int lowestScore;
    bool firstScore{true};
    // stream to write to external file
    ofstream output(fileName);

    // loop through key value pairs in high score map
    for (auto const &[key, val] : highScores)
    {
        if (firstScore)
        { // set the first pair in the map to lowest for comparison
            lowestScore = val;
            lowestUser = key;
            firstScore = false;
        }
        else if (val < lowestScore)
        { // then check if the other pairs are lower
            lowestScore = val;
            lowestUser = key;
            //cerr << lowestScore << endl;
        }
    }

    // check if the user's score qualifies to replace the top 3 scores
    if (score > lowestScore)
    {
        // remove the lowest score in the map
        // NOTE: if user name same as lowest user's name, will remove 3rd top score
        highScores.erase(lowestUser);
        // add the user score to the map
        highScores[userName] = score;
    }

    // loop through the new list of high scores (including user's, if they qualified)
    // sort later
    for (auto const &[key, val] : highScores)
    {
        // display AND write to file
        output << key << "\n" << val << endl;
        cout << key << "\t" << val << endl;
    }
    // close output stream
    output.close();
}

auto border() -> void
{
    //hard code instead of for loop because for loop makes it buggy/choppy output screen
    cout << "_____________________________________________________________________ \n" << flush;
    cout << "|                                                                    |\n" << flush;
    cout << "|                                                                    |\n" << flush;
    cout << "|                                                                    |\n" << flush;
    cout << "|                                                                    |\n" << flush;
    cout << "|                                                                    |\n" << flush;
    cout << "|                                                                    |\n" << flush;
    cout << "|                                                                    |\n" << flush;
    cout << "|                                                                    |\n" << flush;
    cout << "|                                                                    |\n" << flush;
    cout << "|                                                                    |\n" << flush;
    cout << "|                                                                    |\n" << flush;
    cout << "|                                                                    |\n" << flush;
    cout << "|                                                                    |\n" << flush;
    cout << "|                                                                    |\n" << flush;
    cout << "|                                                                    |\n" << flush;
    cout << "|                                                                    |\n" << flush;
    cout << "|                                                                    |\n" << flush;
    cout << "|                                                                    |\n" << flush;
    cout << "|                                                                    |\n" << flush;
    cout << "|                                                                    |\n" << flush;
    cout << "|                                                                    |\n" << flush;
    cout << "|                                                                    |\n" << flush;
    cout << "|                                                                    |\n" << flush;
    cout << "|                                                                    |\n" << flush;
    cout << "|                                                                    |\n" << flush;
    cout << "|                                                                    |\n" << flush;
    cout << "|                                                                    |\n" << flush;
    cout << "|                                                                    |\n" << flush;
    cout << "|                                                                    |\n" << flush;
    cout << "|                                                                    |\n" << flush;
    cout << "--------------------------------------------------------------------- " << flush;
}

auto main() -> int
{

    // Set Up the system to receive input
    SetupScreenAndInput();
    // Check that the terminal size is large enough for the game to be played
    const positionStruct TERMINAL_SIZE{GetTerminalSize()};

    //cerr << TERMINAL_SIZE.row << endl;
    // check if terminal size too small; if so initiate exit
    if ((TERMINAL_SIZE.row < 35) or (TERMINAL_SIZE.col < 70))
    {
        ShowCursor();
        TeardownScreenAndInput();
        cout << endl
             << "Terminal window must be at least 35 by 70 to run this game" << endl;
        return EXIT_FAILURE;
    }

    // need to reset input to allow cin to process correctly (default input mode)
    TeardownScreenAndInput();
    // clear screen and move cursor to top for correct menu formatting
    ClearScreen();
    MoveTo(1, 1);
    string userName{"anonymous"}; // default value for user name
    // display start menu
    bool startGame{StartMenu(userName)};
    if (not startGame)
    { // user didn't want to start (chose to quit in menu)
        // execute exit sequence
        ShowCursor();
        TeardownScreenAndInput();
        cout << endl
             << "sorry to see you go! :(" << endl;
        return EXIT_FAILURE;
    }
    // set up screen again to prep input mode for game
    SetupScreenAndInput();

    vector<string> wordList;
    //loads text file with a bunch of words into vector
    LoadWordList("wordlist.txt", wordList);

    // State Variables
    wordvector activeWords;
    unsigned int ticks{0};
    unsigned int score{0};
    unsigned int lives{5};
    bool quit{false};
    char currentChar;
    string currentCommand;

    bool allowBackgroundProcessing{true};

    auto startTimestamp{chrono::steady_clock::now()};
    auto endTimestamp{startTimestamp};
    int elapsedTimePerTick{100}; // every 0.1s check on things

    SetNonblockingReadState(allowBackgroundProcessing);
    ClearScreen();
    HideCursor();

    // keep looping (game is active) while user hasn't typed "quit" and they haven't died
    while ((not quit) and (lives > 0))
    {

        endTimestamp = chrono::steady_clock::now();
        auto elapsed{chrono::duration_cast<chrono::milliseconds>(endTimestamp - startTimestamp).count()};
        // We want to process input and update the world when EITHER
        // (a) there is background processing and enough time has elapsed
        // (b) when we are not allowing background processing.
        if ((allowBackgroundProcessing and (elapsed >= elapsedTimePerTick)))
        {

            ticks += 1;
            if (ticks % 15 == 0)
            { // only create words every 15 ticks to space them out
                CreateWord(activeWords, wordList);
            }

            UpdateWordColumnPositions(activeWords);

            if (currentChar != '\n' and read(0, &currentChar, 1) == 1) //checks if new char is added and if isn't an \n
            {
                currentCommand += currentChar;
            }
            if (currentChar == '\n') //clears currentcommand if the user enters \n
            {
                // remove \n from command (has whitespace)
                currentCommand.pop_back();
                // check if command matched in activeWords
                CheckMatchedWord(activeWords, currentCommand, quit);
                //cerr << currentCommand << endl;
                currentCommand.clear();
            }

            // add pause logic (toggle moving)

            // clear any activeWords that were matched/are past boundary
            ClearWords(activeWords, score, lives);

            // Clear inputs in preparation for the next iteration
            startTimestamp = endTimestamp;
            currentChar = NULL_CHAR;

            ClearScreen();
            HideCursor();
            // redisplay active words since they've been updated/cleared
            MoveTo(0, 0);
            border();
            DisplayWords(activeWords);
            // shows stats at bottom of terminal
            MoveTo(TERMINAL_SIZE.row, 0);
            ShowStats(score, lives);
            // command displayed just above stats
            MoveTo(TERMINAL_SIZE.row - 2, 1);
            cout << "Command: " << currentCommand;
            ShowCursor();
        }
    }

    // Tidy Up and Close Down
    ClearScreen();
    MoveTo(0, 0); // move cursor back to top of screen
    ShowCursor();
    SetNonblockingReadState(false); // back to blocking input
    // reset input mode
    TeardownScreenAndInput();
    // display game over, display/store high score list
    ShowEndScreen(score);
    map<string, unsigned int> highScores;
    // loads high score file into map
    LoadHighScores("highscores.txt", highScores);
    ProcessHighScores("highscores.txt", highScores, userName, score);
    cout << endl; // be nice to the next command
    return EXIT_SUCCESS;
}