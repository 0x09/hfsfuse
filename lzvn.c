/*
 * Created..: 31 October 2014
 * Filename.: lzvn.c
 * Author...: Pike R. Alpha
 * Enhancer.: Andy Vandijck
 * Purpose..: Command line tool to LZVN compress / decompress a file.
 */

#include "FastCompression.h"

#include <string.h>
#include <stdio.h>

//==============================================================================

int main(int argc, const char * argv[])
{
	FILE *fp							= NULL;

	unsigned char * fileBuffer			= NULL;
	unsigned char * uncompressedBuffer	= NULL;
	unsigned char * bufend				= NULL;

	unsigned long fileLength = 0;
    unsigned long byteshandled = 0;

	size_t compsize = 0;

	if (argc != 4)
	{
		printf("Usage (encode): %s -e <infile> <outfile>\n", argv[0]);
        printf("Usage (decode): %s -d <infile> <outfile>\n", argv[0]);

        return -1;
	} else {
        if (!strncmp(argv[1], "-e", 2))
        {
#if defined(_MSC_VER) && __STDC_WANT_SECURE_LIB__
            fopen_s(&fp, argv[2], "rb");
#else
            fp = fopen(argv[2], "rb");
#endif

            if (fp == NULL)
            {
                printf("Error: Opening of %s failed... exiting\nDone.\n", argv[2]);

                return -1;
            } else {
                fseek(fp, 0, SEEK_END);
                fileLength = ftell(fp);
                
                printf("fileLength: %ld\n", fileLength);
                
                fseek(fp, 0, SEEK_SET);
                
                fileBuffer = malloc(fileLength);
                
                if (fileBuffer == NULL)
                {
                    printf("ERROR: Failed to allocate file buffer... exiting\nAborted!\n\n");

                    if (fp != NULL)
                    {
                        fclose(fp);
                    }

                    return -1;
                } else {
                    fread(fileBuffer, fileLength, 1, fp);

                    if (fp != NULL)
                    {
                        fclose(fp);
                    }
                    
                    size_t workSpaceSize = lzvn_encode_work_size();
                    printf("workSpaceSize: %ld \n", workSpaceSize);
                    
                    void * workSpace = malloc(workSpaceSize);
                    
                    if (workSpace == NULL)
                    {
                        printf("ERROR: Failed to allocate workspace... exiting\nAborted!\n\n");

                        return -1;
                    } else {
                        printf("workSpace declared\n");
                        
                        if (fileLength > workSpaceSize)
                        {
                            workSpaceSize = fileLength;
                        }
                        
                        uncompressedBuffer = (void *)malloc(workSpaceSize);
                        
                        if (uncompressedBuffer == NULL)
                        {
                            printf("ERROR: Failed to allocate uncompressed buffer... exiting\nAborted!\n\n");

                            return -1;
                        } else {
                            size_t outSize = lzvn_encode(uncompressedBuffer, workSpaceSize, fileBuffer, (size_t)fileLength, workSpace);
                            
                            printf("outSize: %ld\n", outSize);
                            
                            if (workSpace != NULL)
                            {
                                free(workSpace);
                            }
                            
                            if (outSize != 0)
                            {
                                bufend = uncompressedBuffer + outSize;
                                compsize = bufend - uncompressedBuffer;
                                
                                printf("compsize: %ld\n", compsize);
                                
#if defined(_MSC_VER) && __STDC_WANT_SECURE_LIB__
                                fopen_s(&fp, argv[3], "wb");
#else
                                fp = fopen (argv[3], "wb");
#endif
                                if (fp == NULL)
                                {
                                    printf("Error: Opening of %s failed... exiting\nAborted!\n", argv[3]);

                                    return -1;
                                }

                                fwrite(uncompressedBuffer, outSize, 1, fp);

                                if (fp != NULL)
                                {
                                    fclose(fp);
                                }
                            }
                        }

                        if (fileBuffer != NULL)
                        {
                            free(fileBuffer);
                        }

                        if (uncompressedBuffer != NULL)
                        {
                            free(uncompressedBuffer);
                        }
                        
                        return 0;
                    }
                }
            }
        } else if (!strncmp(argv[1], "-d", 2)) {
#if defined(_MSC_VER) && __STDC_WANT_SECURE_LIB__
            fopen_s(&fp, argv[2], "rb");
#else
            fp = fopen(argv[2], "rb");
#endif

            if (fp == NULL)
            {
                printf("Error: Opening of %s failed... exiting\nDone.\n", argv[2]);

                return -1;
            } else {
                fseek(fp, 0, SEEK_END);
                fileLength = ftell(fp);
                    
                printf("fileLength: %ld\n", fileLength);
                    
                fseek(fp, 0, SEEK_SET);
                    
                fileBuffer = malloc(fileLength);
                    
                if (fileBuffer == NULL)
                {
                    printf("ERROR: Failed to allocate file buffer... exiting\nAborted!\n\n");

                    if (fp != NULL)
                    {
                        fclose(fp);
                    }
 
                    return -1;
                } else {
                    fread(fileBuffer, fileLength, 1, fp);

                    if (fp != NULL)
                    {
                        fclose(fp);
                    }

#if defined(_MSC_VER) && __STDC_WANT_SECURE_LIB__
                    fopen_s(&fp, argv[3], "wb");
#else
                    fp = fopen(argv[3], "wb");
#endif

                    if (fp == NULL)
                    {
                        printf("Error: Opening of %s failed... exiting\nDone.\n", argv[3]);

                        return -1;
                    } else {
                        size_t workSpaceSize = lzvn_encode_work_size();

                        if (fileLength > workSpaceSize)
                        {
                            workSpaceSize = fileLength;
                        }

                        printf("workSpaceSize: %ld \n", workSpaceSize);
                        uncompressedBuffer = malloc(workSpaceSize);

                        if (uncompressedBuffer == NULL)
                        {
                            printf("ERROR: Failed to allocate uncompressed buffer... exiting\nAborted!\n\n");

                            return -1;
                        } else {
                            while (1)
                            {
                                compsize = lzvn_decode(uncompressedBuffer, workSpaceSize, fileBuffer, fileLength);
                                if(compsize == 0)
                                {
                                    printf("ERROR: Decompression errored out (truncated input?)... exiting\nAborted!\n\n");
                                    return -1;
                                }
                                if(compsize < workSpaceSize)
                                {
                                    fwrite(uncompressedBuffer, 1, compsize, fp);
                                    break;
                                }
                                workSpaceSize *= 2;
                                printf("workSpaceSize: %ld \n", workSpaceSize);
                                uncompressedBuffer = realloc(uncompressedBuffer, workSpaceSize);
                                if (uncompressedBuffer == NULL)
                                {
                                    printf("ERROR: Failed to allocate uncompressed buffer... exiting\nAborted!\n\n");
                                    return -1;
                                }
                            }

                            if (fileBuffer != NULL)
                            {
                                free(fileBuffer);
                            }

                            if (uncompressedBuffer != NULL)
                            {
                                free(uncompressedBuffer);
                            }

                            printf("Uncompressed size: %ld\n", ftell(fp));

                            if (fp != NULL)
                            {
                                fclose(fp);
                            }
                                
                            return 0;
                        }
                    }
                }
            }
        }
    }

	return -1;
}
