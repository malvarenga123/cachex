#include <iostream>
#include <cstdlib>
#include "../src/result.h"
#include "../src/scsi_status.h"

void assert_test(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "Assertion failed: " << message << std::endl;
        std::exit(1);
    }
}

int main() {
    // Case 1: Valid = true, ScsiStatusCode = GOOD
    {
        CommandResult res(0);
        res.Valid = true;
        res.ScsiStatusCode = ScsiStatus::GOOD;
        assert_test(static_cast<bool>(res) == true, "Valid=true, ScsiStatusCode=GOOD should be true");
    }

    // Case 2: Valid = false, ScsiStatusCode = GOOD
    {
        CommandResult res(0);
        res.Valid = false;
        res.ScsiStatusCode = ScsiStatus::GOOD;
        assert_test(static_cast<bool>(res) == false, "Valid=false, ScsiStatusCode=GOOD should be false");
    }

    // Case 3: Valid = true, ScsiStatusCode = CHECK_CONDITION (not GOOD)
    {
        CommandResult res(0);
        res.Valid = true;
        res.ScsiStatusCode = ScsiStatus::CHECK_CONDITION;
        assert_test(static_cast<bool>(res) == false, "Valid=true, ScsiStatusCode=CHECK_CONDITION should be false");
    }

    // Case 4: Valid = false, ScsiStatusCode = CHECK_CONDITION (not GOOD)
    {
        CommandResult res(0);
        res.Valid = false;
        res.ScsiStatusCode = ScsiStatus::CHECK_CONDITION;
        assert_test(static_cast<bool>(res) == false, "Valid=false, ScsiStatusCode=CHECK_CONDITION should be false");
    }

    std::cout << "All tests passed!" << std::endl;
    return 0;
}
