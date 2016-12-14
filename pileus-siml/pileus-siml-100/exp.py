import random

filp = open('expiration.txt','a')
for i in range(1501, 2002):
    rand = random.randint(1,3) 
    num = 999
    filp.write('%s\t%s\n'%(i, num))
filp.close()
