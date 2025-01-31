import numpy as np
import random
import matplotlib.pyplot as plt

class Node:
    def __init__(self, point):
        self.point = np.array(point)  # Including yaw as the third dimension
        self.parent = None
        self.cost = 0

def rrt_star(start, goal, obstacles, dim_x=10, dim_y=10, max_iter=1000, step_size=0.1, radius=2, tolerance=0.3):
    tree = [Node(start)]
    goal_reached_nodes = []  # Keep track of nodes that reach the goal
    path = None
    
    for _ in range(max_iter):
        rand_point = sample_space(dim_x, dim_y)
        nearest_node = find_nearest(tree, rand_point)
        new_node = steer(nearest_node, rand_point, step_size)
        
        if not collides(new_node.point[:2], obstacles):  # Pass only x and y components
            nearby_nodes = find_nearby(tree, new_node, radius)
            new_node = choose_parent(new_node, nearby_nodes)
            tree.append(new_node)
            rewire(tree, new_node, nearby_nodes, radius)
            
            if np.linalg.norm(new_node.point[:2] - goal[:2]) <= tolerance:  # Adjusted condition
                print("Goal reached!")

                # Backtrack with the node having the smallest cost among those that reached the goal
                goal_reached_nodes.append(new_node)
                min_cost_node = min(goal_reached_nodes, key=lambda node: node.cost)
                path, next_best_node = backtrack_path_node(min_cost_node)
                
                # Calculate yaw angles for each node based on the path
                for i in range(len(path)-1):
                    path[i][2] = calculate_yaw_angle(Node(path[i]), Node(path[i+1]))
                    if i == len(path)-1:
                        path[i+1][2] = goal[2:]
    
    return tree, path


def sample_space(dim_x,dim_y):
    rand_x = random.random() * dim_x #* 2 * dim_x - dim_x
    rand_y = random.random() * dim_y #* 2 * dim_y - dim_y
    return np.array([rand_x, rand_y])

def find_nearest(tree, point):
    distances = [np.linalg.norm(node.point[:2] - point[:2]) for node in tree]  # Consider only x and y components
    nearest_index = np.argmin(distances)
    return tree[nearest_index]

def steer(from_node, to_point, step_size):
    if np.linalg.norm(to_point - from_node.point[:2]) < step_size:
        new_point = to_point
    else:
        direction = (to_point - from_node.point[:2]) / np.linalg.norm(to_point - from_node.point[:2])
        new_point = from_node.point[:2] + step_size * direction
    return Node(np.concatenate([new_point, from_node.point[2:]]))  # Preserve yaw angle

def collides(point, obstacles):
    for obstacle in obstacles:
        if np.linalg.norm(point[:2] - obstacle['point']) < obstacle['radius'] + 0.5:
            return True
    return False

def find_nearby(tree, point, radius):
    tree_points = np.array([node.point for node in tree])
    distances = np.linalg.norm(tree_points[:, :2] - point.point[:2], axis=1)  # Consider only x and y components
    nearby_nodes = [node for node, dist in zip(tree, distances) if dist < radius]
    return nearby_nodes

def choose_parent(point, nearby_nodes):
    min_cost = float('inf')
    parent = None
    for node in nearby_nodes:
        # Include yaw angle difference in the cost computation
        cost = node.cost + np.linalg.norm(node.point[:2] - point.point[:2])
        if cost < min_cost:
            min_cost = cost
            parent = node
    point.parent = parent
    point.cost = min_cost
    return point

def rewire(tree, new_node, nearby_nodes, radius):
    for node in nearby_nodes:
        if node.cost > new_node.cost + np.linalg.norm(node.point[:2] - new_node.point[:2]):
            node.parent = new_node
            node.cost = new_node.cost + np.linalg.norm(node.point[:2] - new_node.point[:2])

def backtrack_path_node(node):
    path = []
    next_best_node = node
    while node:
        path.append(node.point)
        node = node.parent
        if node:
            if node.parent: # Exclude root node
                if node.cost < next_best_node.cost:
                    next_best_node = node
    path.reverse()
    return path, next_best_node

def calculate_yaw_angle(node1, node2):
    dx = node2.point[0] - node1.point[0]
    dy = node2.point[1] - node1.point[1]
    return np.arctan2(dy, dx)

def plot_tree(tree, start, goal, obstacles, path=None):
    plt.figure()
    # Plot tree edges
    for node in tree:
        if node.parent:
            plt.plot([node.point[0], node.parent.point[0]], [node.point[1], node.parent.point[1]], 'k-', linewidth=0.5)
    # Plot obstacles
    for obstacle in obstacles:
        circle = plt.Circle(obstacle['point'][:2], obstacle['radius'], color='r')
        plt.gca().add_patch(circle)
    # Plot start and goal points
    plt.scatter(start[0], start[1], color='g', marker='o', label='Start')
    plt.scatter(goal[0], goal[1], color='b', marker='o', label='Goal')
    # Plot final path (if provided)
    if path:
        plt.plot([p[0] for p in path], [p[1] for p in path], 'b-', linewidth=2, label='Path')
    # Labels and formatting
    plt.xlabel('X')
    plt.ylabel('Y')
    plt.title('RRT*')
    plt.legend()
    plt.axis('equal')
    plt.show()

'''
# Example usage
start = np.array([1, 1, 0])
goal = np.array([9, 9, 0])
dim_x = 10
dim_y = 10
obstacles = [{'point': np.array([5, 5]), 'radius': 1}]
tree, path = rrt_star(start, goal, obstacles, dim_x=dim_x, dim_y=dim_y, step_size=0.5, radius=2)
#print(path)
plot_tree(tree, start, goal, obstacles, path)
'''