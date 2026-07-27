int exception(){
  try {
    try {
      throw "1";
    } catch (const char* s) {
      throw "2";
    }
  } catch (const char* s) {
    throw "3";
  }
  return 0;
}
int main() {
  try {
    exception();
  } catch (const char* s) {
    
  }
  return 123;
}