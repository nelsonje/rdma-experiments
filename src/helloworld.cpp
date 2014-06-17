
#include <Verbs.hpp>

using namespace RDMA;

int main( int argc, char * argv[] ) {
  with_verbs_do( &argc, &argv, [] ( Verbs & ib ) {
      LOG(INFO) << "Hello!";
    } );
  return 0;
}
