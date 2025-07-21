#include <iostream>
#include <utility>
#include <cmath>

// Test function for calculateOptimalLayout
std::pair<int, int> calculateOptimalLayout(int numUsers) {
    // Returns (cols, rows) for optimal grid layout
    switch (numUsers) {
        case 1:
            return {1, 1};  // Full screen
        case 2:
            return {2, 1};  // Half-half (side by side)
        case 3:
        case 4:
            return {2, 2};  // 2x2 grid
        case 5:
        case 6:
            return {3, 2};  // 3x2 grid
        case 7:
        case 8:
        case 9:
            return {3, 3};  // 3x3 grid
        case 10:
        case 11:
        case 12:
            return {4, 3};  // 4x3 grid
        case 13:
        case 14:
        case 15:
        case 16:
            return {4, 4};  // 4x4 grid
        case 17:
        case 18:
        case 19:
        case 20:
            return {5, 4};  // 5x4 grid
        case 21:
        case 22:
        case 23:
        case 24:
            return {6, 4};  // 6x4 grid
        default:
            // For more than 24 users, calculate dynamically
            int cols = static_cast<int>(std::ceil(std::sqrt(numUsers)));
            int rows = static_cast<int>(std::ceil(static_cast<double>(numUsers) / cols));
            return {cols, rows};
    }
}

int main() {
    std::cout << "Testing layout calculation for different user counts:\n" << std::endl;
    
    // Test specific user counts mentioned in requirements
    int testCases[] = {1, 2, 4, 6, 9, 12, 16, 20, 24, 25, 30, 36, 50};
    
    for (int userCount : testCases) {
        auto layout = calculateOptimalLayout(userCount);
        int cols = layout.first;
        int rows = layout.second;
        int totalCells = cols * rows;
        
        std::cout << "Users: " << userCount 
                  << " -> Grid: " << cols << "x" << rows 
                  << " (total cells: " << totalCells << ")";
        
        if (totalCells >= userCount) {
            std::cout << " ✓";
        } else {
            std::cout << " ✗ ERROR: Not enough cells!";
        }
        std::cout << std::endl;
    }
    
    return 0;
}