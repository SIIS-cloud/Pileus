import string
import random

high = 150 
low = 150

filp = open('pollution.txt','r')
rev = open('revocation.txt', 'w')
exp = open('expiration.txt', 'w')

current_low = 0
current_high = 0
avg = 0
for line in filp:
    l = line.split()
    num = string.atoi(l[0])
    node = string.atof(l[1])
    if num <= low:
        rev.write('%s\t%s\n'%(num, node))
        exp.write('%s\t%s\n'%(num, node))
        if num == low:
            current_low = node
    if num > low and num <= high:
        exp.write('%s\t%s\n'%(num, node))
        if num == high:
            current_high = node
        avg = avg + node * 1.0 / (high - low)
    if num > high:
        exp.write('%s\t%s\n'%(num, current_high + random.randint(1,3)))

rev.close()
exp.close()

rev = open('revocation.txt', 'a')
for i in range(low+1, high+1):
    rev.write('%s\t%s\n'%(i, current_low + (avg-current_low)/(high-low)*(i-low)))
for i in range(high+1, 3000):
    rev.write('%s\t%s\n'%(i, avg + random.randint(1,3)))
rev.close()
