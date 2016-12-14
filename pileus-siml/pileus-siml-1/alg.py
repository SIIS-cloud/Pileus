import random

filp = open('algorithm.txt','a')
for i in range(1, 2000):
    num = 5 
    filp.write('%s\t%s\n'%(i, num))
filp.close()
