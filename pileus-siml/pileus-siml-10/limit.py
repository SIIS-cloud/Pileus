import random

filp = open('limit.txt','wa')
for i in range(0, 2000):
    num = 1000 
    filp.write('%s\t%s\n'%(i, num))
filp.close()
