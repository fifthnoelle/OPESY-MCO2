#include <iostream>
#include <string>
#include <limits>
using namespace std;

int main() {
    ios::sync_with_stdio(false);
    cin.tie(nullptr);

    string var;
    string val;

    cout << "Enter variable name: " << flush;
    if (!(cin >> var)) {
        cout << "bad var input\n";
        return 1;
    }

    cout << "Enter value: " << flush;
    if (!(cin >> val)) {
        cout << "bad value input\n";
        return 1;
    }

    cout << "\nVariable '" << var << "' = " << val << " declared successfully.\n";
    cin.ignore(numeric_limits<streamsize>::max(), '\n');
}