import sys
import numpy as np
import pandas as pd
import joblib
from sklearn.neighbors import BallTree

fn='worldcities-geo-simp.csv'
df_cities = pd.read_csv(fn,usecols=["cityname", "latitude", "longitude"])
coords_rad = np.deg2rad(df_cities[["latitude", "longitude"]].values)
tree = BallTree(coords_rad, metric="haversine")
joblib.dump(tree, "spatial_index.pkl")
df_cities.to_feather("cities_metadata.feather")

# Testing...

tree = joblib.load("spatial_index.pkl")
df_cities = pd.read_feather("cities_metadata.feather")
target_rad = np.deg2rad([[47.6, -122.33]])
_, index = tree.query(target_rad, k=1)
print(f"Closest City: {df_cities.iloc[index[0][0]]['cityname']}")
