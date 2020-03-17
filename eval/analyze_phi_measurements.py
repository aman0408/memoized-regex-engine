# -*- coding: utf-8 -*-
"""analyze-phi-measurements.ipynb

Automatically generated by Colaboratory.

Original file is located at
    https://colab.research.google.com/drive/17k-4omC6uW46Hbo36iQ9U78wRYyxbJ87

# Mount your drive to access data
"""

try:
  import google.colab
  IN_COLAB = True
except:
  IN_COLAB = False

if IN_COLAB:
  from google.colab import drive
  drive.mount('/content/gdrive')

"""# Analyses for the Regex Optimization project

### Imports
"""

# Commented out IPython magic to ensure Python compatibility.
import os
import json
import re
import itertools
import pandas as pd
import numpy as np
import scipy.stats as stats
import seaborn as sns
import matplotlib
import matplotlib.pyplot as plt
import matplotlib.transforms as transforms

sns.set_style('whitegrid')
# %matplotlib inline

pd.set_option('display.max_columns', 30)

"""## Globals"""

ANALYSIS_ROOT_GOOGLE = os.path.join(os.sep, 'content', 'gdrive', 'Shared drives', 'Regexes and REDOS', 'Memoization')

if IN_COLAB:
  ANALYSIS_ROOT = ANALYSIS_ROOT_GOOGLE
else:
  ANALYSIS_ROOT = '' # Must run in the right directory

DATA_PATH = os.path.join(ANALYSIS_ROOT, 'data')
FIG_PATH = os.path.join(ANALYSIS_ROOT, 'figs')
FIG_FILE_FORMAT = 'pdf'

# Create fig directory structure
try:
  os.mkdir(os.path.join(FIG_PATH))
except:
  pass

"""## Load data"""

def loadNDJSON(filepath):
    dat = []
    with open(filepath, 'r') as infile:
        for line in infile:
            d = json.loads(line)
            dat.append(d)
    return dat
            
allFile = os.path.join(DATA_PATH, 'LF-phiMeasurements.json')
slFile = os.path.join(DATA_PATH, 'sl-phiMeasurements.json')

allReg = loadNDJSON(allFile)
print('Loaded %d measurements for "all" regexes' % (len(allReg)))

slReg = loadNDJSON(slFile)
print('Loaded %d measurements for "SL" regexes' % (len(slReg)))

"""## Munge into DF"""

## Raw values
measure2name = {
    'memoAll':      r'$|Q|$',
    'memoInDegGT1':  r'$|\Phi_{in-degree > 1}|$',
    'memoLoop':   r'$|\Phi_{quantifier}|$',
}
rows = []
for regexType, measurements in [("All", allReg), ("SL", slReg)]:
  for dat in measurements:
    for measure, name in measure2name.items():
      rows += [
             [regexType, measure2name[measure], dat['policy2nSelectedVertices'][measure]]
      ]

dfRaw = pd.DataFrame(
    data=rows,
    columns=['Regex type', 'Measure', 'Value'],
    )

## Ratios
rat2name = {
    'indeg2Q':    r'$\frac{|\Phi_{in-degree > 1}|}{|Q|}$',
    'loop2Q':     r'$\frac{|\Phi_{quantifier}|}{|Q|}$',
    'loop2indeg': r'$\frac{|\Phi_{quantifier}|}{|\Phi_{in-degree > 1}|}$',
}
rows = []
for regexType, measurements in [("All", allReg), ("SL", slReg)]:
  for dat in measurements:
    rat1 = dat['policy2nSelectedVertices']['memoInDegGT1'] / dat['policy2nSelectedVertices']['memoAll']
    rows += [[regexType, rat2name['indeg2Q'], rat1]]
    
    rat2 = dat['policy2nSelectedVertices']['memoLoop']     / dat['policy2nSelectedVertices']['memoAll']
    rows += [[regexType, rat2name['loop2Q'], rat2]]
    
    if dat['policy2nSelectedVertices']['memoInDegGT1'] > 0:
      rat3 = dat['policy2nSelectedVertices']['memoLoop']     / dat['policy2nSelectedVertices']['memoInDegGT1']
      rows += [[regexType, rat2name['loop2indeg'], rat3]]
dfRatios = pd.DataFrame(data=rows, columns=['Regex type', 'Measure', 'Value'])

"""## Summarize DF"""

pd.set_option('display.max_columns', 30)
print("\n*******\n\n  Summary of raw values:\n\n")
print(dfRaw.groupby(['Regex type', 'Measure']).describe())

print("\n*******\n\n  Summary of ratios:\n\n")
print(dfRatios.groupby(['Regex type', 'Measure']).describe())

"""## Plot"""

font = {'family' : 'normal',
        'weight' : 'normal',
        'size'   : 14}
matplotlib.rc('font', **font)
matplotlib.rc('text.latex', preamble=r'\usepackage{sfmath}')
matplotlib.rc('mathtext', fontset='stix')

plt.figure(1)
rawPlt_whis = [1,99]
rawPlt_showfliers = False
rawPlt_fname = os.path.join(FIG_PATH, 'vertex-set-sizes-whis{}-{}-fliers{}.{}'.format(rawPlt_whis[0], rawPlt_whis[1], rawPlt_showfliers, FIG_FILE_FORMAT))
ax = sns.boxplot(x="Measure", y="Value", hue="Regex type", data=dfRaw,
                 #width=1.0,
                 whis=rawPlt_whis, showfliers=rawPlt_showfliers
                #, palette="Set3"
                 )
plt.title('Sizes of selected vertex-sets', fontsize=20)
plt.xticks(fontsize=20)
plt.yticks(fontsize=16)
plt.tight_layout()
print("Saving to {}".format(rawPlt_fname))
plt.savefig(fname=rawPlt_fname)

plt.figure(2)
ratiosPlt_whis = [1,99]
ratiosPlt_showfliers = False
ratiosPlt_fname = os.path.join(FIG_PATH, 'vertex-sizes-ratios-whis{}-{}-fliers{}.{}'.format(ratiosPlt_whis[0], ratiosPlt_whis[1], ratiosPlt_showfliers, FIG_FILE_FORMAT))
ax = sns.boxplot(x="Measure", y="Value", hue="Regex type", data=dfRatios,
                 #width=1.0,
                 whis=ratiosPlt_whis, showfliers=ratiosPlt_showfliers,
                 )
plt.title('Space reduction via selective memoization', fontsize=20)
plt.xticks(fontsize=24)
plt.yticks(fontsize=16)
plt.tight_layout()
print("Saving to {}".format(ratiosPlt_fname))
plt.savefig(fname=ratiosPlt_fname)