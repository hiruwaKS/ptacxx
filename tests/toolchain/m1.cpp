 #include <cstdio>
 #include <vector>
 #include <numeric>
 
 extern "C" {
     int c_get_value(void);
     const char* c_get_string(void);
     void c_print_summary(int, int, const char*);
 }
 
 int main() {
     std::vector<int> v(10);
     std::iota(v.begin(), v.end(), 1);
     int sum = std::accumulate(v.begin(), v.end(), 0);
 
     int val = c_get_value();
     const char* str = c_get_string();
     c_print_summary(sum, val, str);
     return 0;
 }
