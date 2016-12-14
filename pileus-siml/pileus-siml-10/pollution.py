import operator
import sys

# number of nodes
total_m = 1000 
# number of pick
total_b = 5
# number of times
total_r = 3000
# number of clients
total_n = 1000

# cache[1...r]
# cache[1...r]
# ....
# m cache[1...r]
cache = [([-1.0]* (total_r+1)) for i in range(total_m+1)]


def c(n,k): 
    if n == 0 or k == 0:
        return 1
    else:
        return  reduce(operator.mul, range(n - k + 1, n + 1)) / reduce(operator.mul, range(1, k +1))  


constant = c(total_m, total_b)

def function(m, m_p, b, r):
    if m_p < b:
        return 0
    if r*b < m_p: 
        return 0
    if m_p == b and r ==1:
        return 1
    x = min(b, m_p - b)
    result = 0
    for i in range(0, x+1):
        f = 0 
        if cache[m_p-i][r-1] != -1.0:
            f = cache[m_p-i][r-1]
        else:
            f = function(m, m_p-i, b, r-1)
            cache[m_p-i][r-1] = f
        a = c(m_p-i, b-i)*c(m-m_p+i, i)*1.0/constant
        result = result + a*f
    return result

for i in range(1, total_r+1):
    for j in range(total_b, total_m+1):
        result = function(total_m, j, total_b, i)
        cache[j][i] = result
        #print 'cache[%s][%s] = %s'%(j, i, result)

#print cache[total_m][total_r]
#print cache
filp = open('result.txt','w')
for i in range(1, total_r+1):
    exp = 0
    for j in range(total_b, total_m+1):
        if cache[j][i] == -1:
            continue
        exp = exp + cache[j][i] * j
    filp.write('%s\t%s\n'%(i, exp))
filp.close()
