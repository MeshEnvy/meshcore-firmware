#include "zopfli.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc,char **argv) {
  if(argc!=4)return 2;
  FILE *in=fopen(argv[1],"rb");if(!in)return 3;
  fseek(in,0,SEEK_END);long len=ftell(in);rewind(in);
  unsigned char*src=malloc((size_t)len);if(!src)return 4;
  if(fread(src,1,(size_t)len,in)!=(size_t)len)return 5;fclose(in);
  ZopfliOptions options;ZopfliInitOptions(&options);options.numiterations=atoi(argv[3]);
  unsigned char*out=NULL;size_t out_size=0;
  ZopfliCompress(&options,ZOPFLI_FORMAT_DEFLATE,src,(size_t)len,&out,&out_size);
  FILE *dest=fopen(argv[2],"wb");if(!dest)return 6;
  if(fwrite(out,1,out_size,dest)!=out_size)return 7;fclose(dest);
  printf("%ld -> %zu\n",len,out_size);free(src);free(out);return 0;
}
